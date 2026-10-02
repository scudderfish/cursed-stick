// main.cpp — Gotek WiFi "USB stick" (ESP32-S3, Arduino framework).
//
// Presents a read-only FAT16 image (built in PSRAM from LittleFS contents) to
// the Gotek over the native USB OTG port, and serves a tiny HTTP upload UI on
// WiFi so ADF files can be pushed from a PC.
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

#include "fat16.h"

#include "secrets.h"  // WIFI_SSID, WIFI_PASS
// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------
#define HOSTNAME    "cursed"
#define MAX_FILES   64

// ---------------------------------------------------------------------------
// USB mass storage (read-only)
// ---------------------------------------------------------------------------
USBMSC usb_msc;
static uint8_t* fatImage = nullptr;  // fat16::TOTAL_BYTES, allocated from PSRAM

// Sector traffic counters, reported over UART (see loop()). A host that has
// mounted the stick reads sectors; a host that mounted and then failed shows
// as few/no reads.
static volatile uint32_t g_mscReads = 0;
static volatile uint32_t g_mscWrites = 0;

static int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  uint32_t byte = lba * fat16::BYTES_PER_SECTOR + offset;
  if (byte + bufsize > fat16::TOTAL_BYTES) return -1;
  g_mscReads++;
  memcpy(buffer, fatImage + byte, bufsize);
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
static String        g_names[MAX_FILES];   // basename only, for FAT dir entries
static String        g_paths[MAX_FILES];   // full LittleFS path, for open()
static uint32_t      g_count = 0;
static fat16::FileEntry g_entries[MAX_FILES];
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

// Rebuild the PSRAM FAT image from everything currently in LittleFS.
// Call only while the USB host is disconnected (or before begin()).
static bool rebuildImage() {
  g_count = 0;
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) return false;
  File f = root.openNextFile();
  while (f && g_count < MAX_FILES) {
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

  uint32_t used = fat16::build(fatImage, g_entries, g_count, littlefsReader, g_paths);
  // Never leave the cached reader handle open: an open File blocks
  // LittleFS.remove() of that same file, which silently defeats the
  // upload rollback and /delete.
  if (g_currentFile) g_currentFile.close();
  g_currentIndex = UINT32_MAX;
  Serial.printf("FAT: build(%u files) -> %s\n", (unsigned)g_count,
                used == fat16::TOTAL_BYTES ? "ok" : "FAILED (over capacity?)");
  return used == fat16::TOTAL_BYTES;
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
  html += "<p>The 6 MiB image holds about six ADFs. Keep at least one on the "
          "stick: with none, the Gotek shows <b>E34</b>.</p>";
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

static bool    uploadOk = false;
static File    uploadFile;
static String  uploadName;

static void handleUploadDone() {
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
    uploadName = u.filename;
    int slash = uploadName.lastIndexOf('/');
    if (slash >= 0) uploadName = uploadName.substring(slash + 1);
    if (uploadName.length() == 0) uploadName = "unnamed.adf";
    uploadFile = LittleFS.open("/" + uploadName, "w");
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();

    // Rebuild while the host is disconnected to avoid torn reads.
    tud_disconnect();
    delay(30);
    uploadOk = rebuildImage();
    if (uploadOk) {
      tud_connect();                       // FlashFloppy re-scans on re-enumeration
    } else {
      bool removed = LittleFS.remove("/" + uploadName);   // roll back; image full
      Serial.printf("UPLOAD: '%s' rejected; rollback remove -> %s\n",
                    uploadName.c_str(), removed ? "ok" : "FAILED");
      rebuildImage();
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
      bool removed = LittleFS.remove(n);
      Serial.printf("DELETE: remove('%s') -> %s\n", n.c_str(), removed ? "ok" : "FAILED");
      rebuildImage();
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
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
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

  fatImage = (uint8_t*)heap_caps_malloc(fat16::TOTAL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fatImage) {
    Serial.println("FATAL: could not allocate 6 MiB PSRAM image (need an 8 MiB PSRAM module)");
    for (;;) delay(1000);
  }

  if (!LittleFS.begin(true)) {
    Serial.println("FATAL: LittleFS mount failed");
    for (;;) delay(1000);
  }

  Serial.println("building initial FAT16 image...");
  ensureDefaultConfig();
  ensureImageACfg();
  if (rebuildImage()) Serial.printf("  done (%u files)\n", g_count);
  else                Serial.println("  empty or over-capacity (rebuild on upload)");

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
