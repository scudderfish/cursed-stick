// main.cpp — Gotek WiFi "USB stick" (ESP32-S3, Arduino framework).
//
// Presents a read-only FAT16 volume, computed on demand from the LittleFS
// contents (no ADF-sized buffer exists anywhere), to the Gotek over the native
// USB OTG port, and serves a tiny HTTP upload UI on WiFi so ADF files can be
// pushed from a PC.
//
// One-way only: the Gotek can never write back (MSC write callback returns -1).
#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>
#include "tusb.h"
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <errno.h>

#include "fat16.h"

#include "secrets.h" // WIFI_SSID, WIFI_PASS
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------
#define HOSTNAME    "cursed"

// Longest filename an upload will try to store: 255 bytes, the VFAT LFN limit —
// which is also what a firmware-formatted LittleFS accepts (measured on
// hardware: 255 stored verbatim, 256 refused). A LittleFS image written by
// `pio run -t uploadfs` is capped at 32 bytes instead (mklittlefs records
// name_max = 32 in the superblock and lfs_mount() adopts it for the life of that
// image), so handleUploadBody() walks progressively smaller budgets and stores
// the longest deterministic shortening the filesystem takes; a name that fits is
// stored exactly as sent.
#define STORAGE_NAME_MAX 255

// ---------------------------------------------------------------------------
// USB mass storage (read-only)
// ---------------------------------------------------------------------------
USBMSC usb_msc;

// The volume layout and the file list it describes. `Volume` is a few hundred
// bytes of placement state: every byte the host reads is computed on demand by
// fat16::readBytes(), so the firmware holds no copy of the ADFs at all (and
// needs no PSRAM). The reader and the path table it opens from are defined with
// the LittleFS code below, and declared here because the MSC callback needs
// them.
static fat16::Volume    g_volume;
static fat16::FileEntry g_entries[fat16::MAX_FILES];
static String           g_paths[fat16::MAX_FILES];
static bool littlefsReader(void* ctx, uint32_t fileIndex, uint32_t offset,
                           uint8_t* dst, uint32_t n);

// The USB task serves sectors from this state while the HTTP task rewrites it on
// every upload or delete. `tud_disconnect()` only stops *new* transfers — a
// callback already running carries on for up to a 64 KiB multi-sector read — so
// both sides take this lock. The USB task can only ever wait for the
// microseconds a relayout takes; the HTTP task for the length of one transfer.
static SemaphoreHandle_t g_volumeLock = nullptr;

struct VolumeLock {
  VolumeLock()  { xSemaphoreTake(g_volumeLock, portMAX_DELAY); }
  ~VolumeLock() { xSemaphoreGive(g_volumeLock); }
};

// Sector traffic counters, reported over UART (see loop()). A host that has
// mounted the stick reads sectors; a host that mounted and then failed shows
// as few/no reads.
static volatile uint32_t g_mscReads = 0;
static volatile uint32_t g_mscWrites = 0;

static int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  uint32_t byte = lba * fat16::BYTES_PER_SECTOR + offset;
  g_mscReads++;
  VolumeLock lock;
  if (!fat16::readBytes(g_volume, g_entries, littlefsReader, g_paths, byte,
                        (uint8_t*)buffer, bufsize)) {
    return -1;
  }
  return (int32_t)bufsize;
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t*, uint32_t bufsize) {
  // Read-only. The volume advertises itself write-protected (see tud_msc_is_writable_cb). 
  // Log the first few attempts so a host that ignores the write-protect bit is visible on the UART.
  uint32_t attempts = ++g_mscWrites;
  if (attempts <= 5 || (attempts % 100) == 0)
    Serial.printf("MSC: rejected write attempt #%u (lba %u, offset %u, %u bytes)\n",
                  attempts, lba, offset, bufsize);
  return -1;                          // read-only
}

// FlashFloppy derives "is this volume read-only?" from the MSC MODE SENSE
// write-protect bit (usbh_msc_readonly() -> USBH_MSC_Param.MSWriteProtect ->
// volume_readonly()), and honours it throughout (fs.c, floppy.c, main.c).
// Without it the Gotek believes the stick is writable, tries to persist
// IMAGE_A.CFG, and gets an I/O error instead of a clean read-only volume.
// TinyUSB builds that bit from this weak hook (class/msc/msc_device.h), so
// overriding it is what makes our read-only design work as intended.
extern "C" bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return false;
}

// ---------------------------------------------------------------------------
// LittleFS -> FAT16 image
// ---------------------------------------------------------------------------
static String        g_names[fat16::MAX_FILES];   // basename only, for FAT dir entries
static uint32_t      g_count = 0;
static File          g_currentFile;
static uint32_t      g_currentIndex = UINT32_MAX;

static bool littlefsReader(void* ctx, uint32_t fileIndex, uint32_t offset,
                           uint8_t* dst, uint32_t n) {
  String* paths = (String*)ctx;
  if (fileIndex != g_currentIndex) {
    if (g_currentFile) g_currentFile.close();
    g_currentFile = LittleFS.open(paths[fileIndex], "r");
    g_currentIndex = fileIndex;
  }
  if (!g_currentFile) return false;
  if (!g_currentFile.seek(offset)) return false;
  return g_currentFile.read(dst, n) == (int)n;
}

// Drop the cached reader handle. The MSC read path leaves the last file the host
// read open (littlefsReader caches it for sequential reads), and an open File
// blocks LittleFS.remove() of that same file — so anything that removes or
// overwrites a file must invalidate the cache first, under the volume lock.
static void closeCachedReader() {
  if (g_currentFile) g_currentFile.close();
  g_currentIndex = UINT32_MAX;
}

// Recompute the volume layout from everything currently in LittleFS. No file
// data is read here (the host pulls that as it asks for sectors), so this is
// instant even with a full stick. Call only while the USB host is disconnected
// (or before begin()).
static bool rebuildVolume() {
  VolumeLock lock;
  g_count = 0;
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) return false;
  File f = root.openNextFile();
  while (f && g_count < fat16::MAX_FILES) {
    if (!f.isDirectory()) {
      g_names[g_count] = f.name();               // basename only, e.g. "FF.CFG"
      g_paths[g_count] = f.path();               // e.g. "/FF.CFG" — name() lacks the '/'
      g_entries[g_count].name = g_names[g_count].c_str();
      g_entries[g_count].size = (uint32_t)f.size();
      g_count++;
    }
    f = root.openNextFile();
  }
  f.close();
  root.close();

  bool ok = fat16::layout(g_entries, g_count, &g_volume);
  closeCachedReader();
  Serial.printf("FAT: layout(%u files, %u root entries) -> %s\n",
                (unsigned)g_count, (unsigned)g_volume.rootEntries,
                ok ? "ok" : "FAILED (over capacity?)");
  return ok;
}

// FlashFloppy reads FF.CFG from the root of the stick to configure the Gotek.
// Ship a sane default so the stick works on a fresh Gotek with no hand-prepared
// config; uploading your own /FF.CFG overwrites this file and takes precedence.
// Values chosen to match FlashFloppy's documented defaults for an Amiga:
//   interface = amiga     (Amiga uses the Amiga bus)
//   host      = unspecified (detection from the image suffix: .ADF -> Amiga)
static const char* DEFAULT_FF_CFG =
    "## FlashFloppy config, written by the cursed-stick firmware.\n"
    "## Upload your own FF.CFG to override this file.\n"
    "interface = amiga\n"
    "display-type = auto\n"
    "chgr-pin = auto\n"
    "disk-change-time-ms = 500\n"
    "nav-mode = native\n";

// First line of every firmware-generated FF.CFG. Any file not starting with
// this marker is treated as user-supplied and never rewritten.
static const char SHIPPED_MARKER[] = "## FlashFloppy config, written by";

static void ensureDefaultConfig() {
  // Regenerate whenever the shipped default changes, so editing
  // DEFAULT_FF_CFG takes effect on the next boot instead of being masked by
  // the file a previous firmware version left on the device. Only files
  // carrying our marker line are touched; a user-uploaded FF.CFG is left
  // alone (it has no marker). Unknown options are silently skipped by
  // FlashFloppy, so a typo here is inert rather than harmful.
  File f = LittleFS.open("/FF.CFG", "r");
  if (f) {
    String existing = f.readString();
    f.close();
    if (!existing.startsWith(SHIPPED_MARKER) || existing == DEFAULT_FF_CFG) return;
  }
  File w = LittleFS.open("/FF.CFG", "w");
  if (!w) return;
  w.print(DEFAULT_FF_CFG);
  w.close();
  Serial.println("wrote default FF.CFG");
}

// FlashFloppy opens IMAGE_A.CFG with FA_OPEN_ALWAYS, but on a read-only volume
// it masks the mode down to FA_READ (src/fs.c: mask_mode()), so the file must
// ALREADY EXIST or the open fails with FR_NO_FILE — which aborts startup and
// shows "*FATFS* 04" on the Gotek's display (src/main.c: handle_errors()).
// A writable stick gets this file created by the Gotek itself; ours is
// read-only by design, so we must ship it. Empty is fine (that is what a Gotek
// leaves behind). It belongs in the same folder as FF.CFG: the FF/ subfolder
// if one exists, otherwise the image root.
static void ensureImageACfg() {
  File f = LittleFS.open("/IMAGE_A.CFG", "r");
  if (f) { f.close(); return; }
  File w = LittleFS.open("/IMAGE_A.CFG", "w");
  if (!w) return;
  w.close();
  Serial.println("wrote empty IMAGE_A.CFG (required on read-only media)");
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
WebServer server(80);

// Escape for HTML text and for a single-quoted attribute. Filenames come from
// multipart uploads, so they are not trusted to be free of markup.
static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    switch (in[i]) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += in[i];
    }
  }
  return out;
}

// Files the stick must always carry: FlashFloppy needs IMAGE_A.CFG to exist on
// a read-only volume (a missing one is fatal -> *FATFS* 04) and FF.CFG configures
// the Gotek. Both are written only in setup(), so losing one breaks the Gotek
// until the next boot. Match on the basename, which is what both the file list
// and /delete carry.
static bool isRequiredFile(const String& name) {
  String base = name.substring(name.lastIndexOf('/') + 1);
  return base == "FF.CFG" || base == "IMAGE_A.CFG";
}

static void handleRoot() {
  String html;
  html.reserve(3072);
  html += "<!doctype html><html><head><meta charset='utf-8'>"
          "<title>Cursed Gotek ADF stick</title>"
          "<style>body{font-family:sans-serif;margin:2rem}"
          "li{margin:.25rem 0}form{display:inline}.req{color:#888}</style></head><body>";
  html += "<h1>Cursed Gotek ADF stick</h1>";
  html += "<p>The volume spans the whole filesystem — about eleven ADFs. Keep at "
          "least one on the stick: with none, the Gotek shows <b>E34</b>.</p>";
  html += "<form method='post' action='/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='file' multiple> <button>Upload</button></form>";
  html += "<h2>Files</h2><ul>";
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String nm = htmlEscape(f.name());
      html += "<li>" + nm + " (" + String(f.size()) + " bytes) ";
      if (isRequiredFile(f.name())) {
        html += "<span class='req'>required</span>";   // /delete refuses these too
      } else {
        html += "<form method='post' action='/delete' "
                "onsubmit=\"return confirm('Delete this file from the stick?');\">";
        html += "<input type='hidden' name='name' value='" + nm + "'>";
        html += "<button type='submit'>delete</button></form>";
      }
      html += "</li>";
    }
    f = root.openNextFile();
  }
  html += "</ul></body></html>";
  server.send(200, "text/html", html);
}

static bool    uploadOk = false;        // image rebuilt with the new file
static bool    uploadFailed = false;    // could not name or store the file
static bool    uploadCreated = false;   // the file exists on the stick
static File    uploadFile;
static String  uploadName;              // name the file is stored (and shown) as
static String  uploadOriginal;          // name the client sent us

static void handleUploadDone() {
  if (uploadFailed) {
    // Never report success for a file that is not on the stick.
    server.send(500, "text/plain", "cannot store '" + uploadOriginal + "'");
    return;
  }
  if (!uploadOk) {
    server.send(507, "text/plain", "image full");
    return;
  }
  // Bounce the browser back to the file list, exactly as /delete does. Tools
  // (tools/adfpush) pass curl -L and so still see GET / -> 200.
  server.sendHeader("Location", "/");
  server.send(302);
}

static void handleUploadBody() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    uploadOk = false;
    uploadFailed = false;
    uploadCreated = false;

    uploadOriginal = u.filename;
    // A multipart filename can carry a path prefix (some clients send the whole
    // client-side path). Keep the basename, whatever separator was used.
    int cut = uploadOriginal.lastIndexOf('/');
    int bslash = uploadOriginal.lastIndexOf('\\');
    if (bslash > cut) cut = bslash;
    if (cut >= 0) uploadOriginal = uploadOriginal.substring(cut + 1);
    if (uploadOriginal.length() == 0) uploadOriginal = "unnamed.adf";
    uploadName = uploadOriginal;

    // Store the name the client sent wherever the filesystem allows it: it does
    // NOT have to be an 8.3 name, because the name the Gotek sees is generated
    // from whatever is stored (rebuildVolume() -> fat16::layout() makes the short
    // name and the VFAT LFN). LittleFS can still refuse a long name outright,
    // though — a filesystem written by `pio run -t uploadfs` keeps its 32-byte
    // name_max for the rest of its life (mklittlefs records it in the superblock
    // and lfs_mount() adopts it, see AGENTS.md) — so fall back to the longest
    // deterministic shortening the filesystem actually accepts.
    errno = 0;
    { VolumeLock lock; closeCachedReader(); }   // no stale handle on a file we overwrite
    uploadFile = LittleFS.open("/" + uploadName, "w");
    if (!uploadFile) {
      Serial.printf("UPLOAD: cannot store '%s' (errno %d%s)\n",
                    uploadOriginal.c_str(), errno,
                    errno == ENAMETOOLONG ? ", name limit" : "");
      // Walk the budgets down: a firmware-formatted filesystem takes up to
      // STORAGE_NAME_MAX, an `uploadfs`-seeded one stops at 32. The first budget
      // that both yields a name and opens it wins, and if none does the "no room"
      // path below fails the upload instead of dropping it silently.
      static const uint32_t budgets[] = { STORAGE_NAME_MAX, 192, 128, 64, 32, 16 };
      char shorter[STORAGE_NAME_MAX + 1];
      for (uint32_t budget : budgets) {
        if (!fat16::storableName(uploadOriginal.c_str(), budget, shorter)) continue;
        uploadName = shorter;
        uploadFile = LittleFS.open("/" + uploadName, "w");
        if (uploadFile) break;
      }
    }
    uploadCreated = uploadFile;      // a false File means every open failed
    if (!uploadCreated) {
      Serial.printf("UPLOAD: no room for '%s' on the stick\n", uploadOriginal.c_str());
      uploadFailed = true;
      return;
    }
    if (uploadName == uploadOriginal)
      Serial.printf("UPLOAD: storing '%s'\n", uploadName.c_str());
    else
      Serial.printf("UPLOAD: storing '%s' as '%s'\n",
                    uploadOriginal.c_str(), uploadName.c_str());
  } else if (u.status == UPLOAD_FILE_WRITE) {
    // A short write means the file on the stick is not the file the client sent.
    if (uploadCreated && uploadFile.write(u.buf, u.currentSize) != u.currentSize) {
      Serial.printf("UPLOAD: short write on '%s'\n", uploadName.c_str());
      uploadFailed = true;
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (uploadCreated) uploadFile.close();

    if (uploadFailed) {
      if (!uploadCreated) {
        // Nothing reached the stick (the name could not be generated or the file
        // could not be created), so the image is untouched.
        Serial.printf("UPLOAD: '%s' not stored; image unchanged\n",
                      uploadOriginal.c_str());
        return;
      }
      // Remove the partial file so nothing half-written is left behind.
      tud_disconnect();
      delay(30);
      bool removed;
      { VolumeLock lock; closeCachedReader(); removed = LittleFS.remove("/" + uploadName); }
      Serial.printf("UPLOAD: '%s' discarded; remove -> %s\n",
                    uploadName.c_str(), removed ? "ok" : "FAILED");
      rebuildVolume();
      tud_connect();
      return;
    }

    // Rebuild while the host is disconnected to avoid torn reads.
    tud_disconnect();
    delay(30);
    uploadOk = rebuildVolume();
    if (uploadOk) {
      tud_connect();                       // FlashFloppy re-scans on re-enumeration
    } else {
      bool removed;
      { VolumeLock lock; closeCachedReader(); removed = LittleFS.remove("/" + uploadName); }
      Serial.printf("UPLOAD: '%s' rejected; rollback remove -> %s\n",
                    uploadName.c_str(), removed ? "ok" : "FAILED");
      rebuildVolume();
      tud_connect();
    }
  }
}

static void handleDelete() {
  if (server.hasArg("name")) {
    String n = "/" + server.arg("name");
    if (isRequiredFile(n)) {
      Serial.printf("DELETE: refused to remove required file '%s'\n", n.c_str());
      server.send(403, "text/plain",
                  n + " is required on the stick and cannot be deleted");
      return;
    }
    if (n.length() > 1 && n.indexOf("..") < 0) {
      tud_disconnect();
      delay(30);
      // The host may have read this very file: drop the cached handle before the
      // remove, or LittleFS refuses to unlink an open file and the disk silently
      // stays on the stick.
      bool removed;
      { VolumeLock lock; closeCachedReader(); removed = LittleFS.remove(n); }
      Serial.printf("DELETE: remove('%s') -> %s\n", n.c_str(), removed ? "ok" : "FAILED");
      rebuildVolume();
      tud_connect();
    }
  } else {
    Serial.println("DELETE: request without a 'name' argument");
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

static void handleNotFound() {
  server.send(404, "text/plain", "not found");
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void connectWiFi() {
  WiFi.mode(WIFI_STA);            // brings the driver up, so setSleep() is valid here
  WiFi.setHostname(HOSTNAME);
  // Modem sleep is off before associating as well as after: WiFi.begin() returns
  // before the 4-way handshake completes, and this is the same setting the
  // gotcha above is about.
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("connecting to WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    // Modem sleep can make an ESP32 miss inbound frames on some APs; we are a
    // server, so keep the radio awake. Without this the stick is unreachable
    // from other subnets while its own outbound connections still work.
    WiFi.setSleep(false);
    // Print the IP as the URL: mDNS (cursed.local) is link-local and only
    // resolves on the same layer-2 network, which is often not the case.
    Serial.printf("\nconnected: http://%s/  (ssid '%s', rssi %d)\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.SSID().c_str(), WiFi.RSSI());
  } else {
    Serial.println("\nWiFi FAILED — upload via IP once you reconnect, or reboot");
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  g_volumeLock = xSemaphoreCreateMutex();
  if (!g_volumeLock) {
    Serial.println("FATAL: could not create the volume lock");
    for (;;) delay(1000);
  }

  if (!LittleFS.begin(true)) {
    Serial.println("FATAL: LittleFS mount failed");
    for (;;) delay(1000);
  }

  Serial.println("building initial volume layout...");
  ensureDefaultConfig();
  ensureImageACfg();
  if (rebuildVolume()) Serial.printf("  done (%u files)\n", g_count);
  else                 Serial.println("  empty or over-capacity (rebuild on upload)");

  usb_msc.vendorID("Pi Gotek");
  usb_msc.productID("ADF Stick");
  usb_msc.productRevision("1.0");
  usb_msc.onRead(mscRead);
  usb_msc.onWrite(mscWrite);
  usb_msc.mediaPresent(true);
  usb_msc.begin(fat16::TOTAL_BYTES / fat16::BYTES_PER_SECTOR, fat16::BYTES_PER_SECTOR);
  USB.begin();

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadBody);
  server.on("/delete", HTTP_POST, handleDelete);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();

  // Report USB host attach/detach and sector traffic. This tells us whether the
  // Gotek has enumerated the stick at all, and whether it is actually reading
  // it (a host that mounted and then gave up shows as no reads).
  static int lastMounted = -1;
  static uint32_t lastReads = 0;
  static uint32_t lastReport = 0;
  int mounted = tud_mounted() ? 1 : 0;
  if (mounted != lastMounted) {
    Serial.printf("USB: %s (%u reads, %u rejected writes)\n",
                  mounted ? "host mounted the device" : "host unmounted the device",
                  g_mscReads, g_mscWrites);
    lastMounted = mounted;
    lastReport = millis();
  } else if (mounted && g_mscReads != lastReads && millis() - lastReport > 5000) {
    Serial.printf("USB: active - %u reads, %u rejected writes\n", g_mscReads, g_mscWrites);
    lastReport = millis();
  }
  lastReads = g_mscReads;

  delay(2);
}
