/*
 * 7segclock (ntp4digit) — an NTP clock on WS2812 seven-segment digits, driven by
 * an ESP32-C3 Super Mini.
 *
 * Joins wifi through a captive setup portal, gets the time by NTP, and serves a
 * settings page plus a wiring editor at http://mini7seg.local/. Updates itself
 * from GitHub Releases (Firmware > Check for updates), takes .bin uploads at
 * /update, and accepts ArduinoOTA. New or unbootable boards are flashed from the
 * browser installer at https://mcyork.github.io/7segclock/.
 *
 * REPOS. github.com/mcyork/7segclock is the only home of this firmware and the
 * only place releases are cut. github.com/mcyork/mini7seg holds the segment
 * library it depends on and the enclosure. To work on both at once, clone them
 * side by side and build with `pio run -e dev`, which takes the library from
 * ../mini7seg instead of the pinned git commit.
 *
 * WIRING  (three wires, all on one edge of the C3 Super Mini)
 *   5V   -> panel H1 V
 *   GND  -> panel H1 G
 *   GPIO -> panel H1 DI        GPIO4 by default; settable on the page
 *
 *   The four digit boards chain H2 (DO) of one into H1 (DI) of the next, so the
 *   panel is one 32-LED strip: digit 0 is LEDs 0-7, digit 3 is LEDs 24-31, each
 *   in segment order A,B,C,D,E,F,G,DP. Anything else is learned on /wiring.
 *
 * ⚠ 3.3 V DATA INTO 5 V LEDS IS OUT OF SPEC.
 *   WS2812 wants V_IH = 0.7 x VDD = 3.5 V; a C3 pin gives 3.3 V. It usually
 *   works, which is what makes it nasty — the failure is intermittent, shows up
 *   as the first digit flickering or the chain freezing, and tracks temperature
 *   and lead length. If it misbehaves, cheapest fixes in order: a series
 *   Schottky in the LED's 5 V feed (drops VDD to ~4.3 V, so V_IH ~3.0 V), a
 *   74AHCT125 buffer, or burn LED 0 as a sacrificial level-shifting pixel.
 *
 * ⚠ THE DATA PIN AVOIDS THE STRAPPING PINS. On the C3, GPIO2, GPIO8 and GPIO9 are
 *   sampled at reset to choose the boot mode. A WS2812 line idles low, and a
 *   strapping pin held the wrong way at power-up puts the chip into download
 *   mode instead of running your sketch — which looks exactly like a dead
 *   board. The allow-list in settings.h withholds them.
 *
 * THE PORTAL IS NEVER A DEAD END, AND NEVER A REFLEX
 *   An early revision parked the clock in the setup portal FOREVER on any NTP
 *   failure or ~40 s of router downtime. A later one still raised the portal on
 *   a cold boot while the router was booting — which after a power cut is the
 *   normal case, and it left an open access point up for a minute or two.
 *   Now: with saved credentials the clock waits before it offers the portal —
 *   three minutes on a boot that never associated, ten after a drop — keeps
 *   showing the time while the portal is up, and keeps retrying the saved
 *   network in the background without blocking the phone.
 *   NTP failure is not a wifi problem and never triggers the portal at all.
 *
 * WRITES NEED POST + A HEADER
 *   Every route that changes state requires POST and `X-7seg: 1`. A page on
 *   another origin can make a browser send a GET or a form POST to this LAN
 *   address, but it cannot add a custom header without a CORS preflight, which
 *   this server never answers. That closes the <img src=/set?pin=0> hole
 *   without a password. There is still no authentication: anyone on your LAN
 *   who opens the page can use it, and the README says so.
 *
 * BUILD  (PlatformIO; every dependency is pinned in platformio.ini)
 *   pio run                                        # build
 *   pio run -t upload                              # flash over USB
 *   pio run -t upload --upload-port mini7seg.local # ArduinoOTA
 *   bun Release.ts --check                         # release chain, see that file
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <FastLED.h>
#include <time.h>
#include <esp_sntp.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include "String7Segment.h"
#include "settings.h"
#include "webui.h"
#include "geometryui.h"
#include "roots.h"

// POWER DIAGNOSTICS
//
// The laptop cannot measure what the board draws -- macOS reports only the USB
// descriptor's declared bMaxPower, which is a request, not a measurement. But
// the question is not really "how many mA", it is "is the supply sagging enough
// to matter", and the ESP32 answers that itself: the brownout detector fires a
// reset with its own reason code. A counter in NVS makes it visible even if
// nobody was watching the serial port when it happened.
inline const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "software";     // our own ESP.restart()
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";     // the one that means power
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT:       return "external";
    default:                return "unknown";
  }
}
uint32_t bootCount = 0, brownoutCount = 0;
uint8_t  lastStressTry = 0;
#include "ticker.h"

// ---------------------------------------------------------------- behaviour
#define FW_VERSION   "1.2.7"
// Self-update from GitHub Releases. The device asks the API for the latest tag,
// and — since 1.2.0 — downloads THAT tag's asset rather than whatever `latest`
// resolves to at flash time, so the version it verified is the version it flashes.
#define GH_REPO      "mcyork/7segclock"
#define GH_LATEST    "https://api.github.com/repos/" GH_REPO "/releases/latest"
#define GH_DL        "https://github.com/" GH_REPO "/releases/download/"

#define AP_PASSWORD  "sevenseg"   // >= 8 chars or the AP silently refuses to start
#define NTP_SERVER   "pool.ntp.org"

#define WIFI_TIMEOUT_MS      15000UL   // one association attempt at boot
#define RECONNECT_EVERY_MS   20000UL   // while online-but-dropped
#define OFFLINE_TO_PORTAL_MS 600000UL  // 10 min down before offering the portal (after a drop)
#define BOOT_OFFLINE_MS      180000UL  // 3 min if this boot has never associated: a router takes about that
                                       // long to come back after a power cut; wrong credentials need the
                                       // portal sooner than a ten-minute wait, and the portal keeps retrying
#define PORTAL_RETRY_MS      60000UL   // portal keeps trying saved creds
#define PORTAL_TRY_MS        12000UL   // ...for this long each time, then goes quiet so the AP can be seen
#define NTP_RETRY_MS         60000UL   // re-kick SNTP while the first sync is outstanding
#define RESYNC_AFTER_MS      21600000UL // re-kick SNTP every 6 h
#define IMAGE_CONFIRM_MS     60000UL   // a fresh OTA image must run this long to stay
#define FRAME_MS             200       // display cadence; loop() itself runs much faster
#define AUTOCHECK_FIRST_MS   60000UL   // first automatic update check, after the time is known
#define AUTOCHECK_EVERY_MS   86400000UL // then once a day
#define AUTOCHECK_RETRY_MS   3600000UL // a failed check is retried in an hour
#define AUTOINSTALL_HOUR     3         // local hour when INSTALL mode installs

#define SSID_MAX  32              // 802.11 limits; longer silently never associates
#define PASS_MAX  63

// ---------------------------------------------------------------- globals
CRGB leds[MAX_LEDS];
Preferences prefs;
WebServer server(80);
DNSServer dns;
CLEDController* ledController = nullptr;

bool portalUp   = false;
volatile bool timeValid = false;  // set by the SNTP callback (tcpip task), never cleared
bool wasOnline  = false;          // edge detector for onOnline()
bool everOnline = false;          // this boot associated at least once
bool bootPending = false;         // image state at boot: pending means the bootloader is tracking us
bool ntpStarted = false;
uint32_t lastConnectedMs = 0;
uint32_t lastReconnectMs = 0;
uint32_t lastPortalTryMs = 0;
uint32_t portalTryUntilMs = 0;    // non-zero while a bounded STA attempt is running under the portal
uint32_t lastNtpTryMs    = 0;
uint32_t lastResyncMs    = 0;
volatile uint32_t lastRealSyncMs = 0;   // stamped ONLY by the SNTP callback (tcpip task)
uint32_t lastPaintMs     = 0;
uint8_t  spinStep        = 0;
Settings cfg;
// Update state, read by the display (flashing dot) and by the update routes.
bool     updAvail = false;        // the last check found a strictly newer release
uint32_t lastCheckMs = 0;         // when the last successful check ran
uint32_t nextAutoCheckMs = 0;     // 0 until the clock first has the time
int      lastInstallYday = -1;    // INSTALL mode tries once per night, not once per loop
uint8_t  bootPin = DEFAULT_DATA_PIN;   // the pin FastLED was built on this boot
char     hostName[DEV_NAME_MAX + 1];
char     apSsid[DEV_NAME_MAX + 8];

/** The allow-list as a JSON array, so the settings page renders exactly the
 *  pins this firmware will actually accept. */
inline String pinListJson() {
  String out;
  for (uint8_t i = 0; i < PIN_COUNT; i++) { if (i) out += ','; out += PIN_LIST[i]; }
  return out;
}

String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 4);
  for (uint16_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

void sendJsonError(uint16_t code, const String& problem) {
  server.send(code, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(problem) + "\"}");
}

// Is the Host header one of OUR names? DNS rebinding lets a hostile page point its
// own domain at this LAN address and become same-origin, at which point it can add
// any header it likes. Writes therefore also have to be addressed to the clock by a
// name the clock knows it has: its IP, its AP IP, `<name>` or `<name>.local`. Router
// DNS suffixes (mini7seg.lan) are deliberately NOT accepted — an allowance for
// "<name>.<anything>" is exactly what an attacker who owns <name>.<tld> would use.
bool hostIsOurs(String h) {
  int c = h.indexOf(':'); if (c >= 0) h = h.substring(0, c);   // strip :port
  h.toLowerCase();
  if (h.endsWith(".")) h.remove(h.length() - 1);
  String n = hostName;
  return h == WiFi.localIP().toString() || h == WiFi.softAPIP().toString() || h == n || h == n + ".local";
}

// CSRF guard. The pages send POST plus `X-7seg: 1` on every write; nothing a
// cross-origin page can produce without a preflight carries that header. The Host
// check closes DNS rebinding. While the portal is up the Host is whatever the phone
// probed for, and the AP has no internet path, so the Host check is skipped there.
bool requireWrite() {
  if (server.method() != HTTP_POST) { sendJsonError(405, "POST required"); return false; }
  if (server.header("X-7seg") != "1") { sendJsonError(403, "missing X-7seg header"); return false; }
  if (!portalUp && !hostIsOurs(server.hostHeader())) { sendJsonError(403, "unexpected Host; use the clock's IP or " + String(hostName) + ".local"); return false; }
  return true;
}

bool parseLongStrict(const String& raw, long& out) {
  const char* p = raw.c_str();
  while (*p == ' ' || *p == '\t') p++;
  if (!*p) return false;
  char* end;
  long v = strtol(p, &end, 10);
  if (end == p) return false;
  while (*end == ' ' || *end == '\t') end++;
  if (*end) return false;
  out = v;
  return true;
}

bool readLongArg(const char* name, long lo, long hi, long& out) {
  if (!server.hasArg(name)) {
    sendJsonError(400, String("missing ") + name);
    return false;
  }
  long v;
  if (!parseLongStrict(server.arg(name), v)) {
    sendJsonError(400, String(name) + " must be an integer");
    return false;
  }
  if (v < lo || v > hi) {
    sendJsonError(400, String(name) + " must be " + lo + ".." + hi);
    return false;
  }
  out = v;
  return true;
}

bool readOptionalU8(const char* name, uint8_t& out, uint8_t lo, uint8_t hi) {
  if (!server.hasArg(name)) return true;
  long v;
  if (!parseLongStrict(server.arg(name), v)) {
    sendJsonError(400, String(name) + " must be an integer");
    return false;
  }
  if (v < lo || v > hi) {
    sendJsonError(400, String(name) + " must be " + lo + ".." + hi);
    return false;
  }
  out = (uint8_t)v;
  return true;
}

bool argIsAllowed(const String& name, const char* const* allowed, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) if (name == allowed[i]) return true;
  return false;
}

bool rejectUnexpectedArgs(const char* const* allowed, uint8_t count) {
  for (uint8_t i = 0; i < server.args(); i++) {
    String name = server.argName(i);
    if (!argIsAllowed(name, allowed, count)) {
      sendJsonError(400, "unexpected argument " + name);
      return false;
    }
  }
  return true;
}

bool rejectNoArgs() {
  return rejectUnexpectedArgs(nullptr, 0);
}

Ticker btc;
uint32_t lastTickMs = 0;
bool webUp = false;

enum PreviewMode : uint8_t { PREVIEW_OFF = 0, PREVIEW_IDENTIFY, PREVIEW_PROBE };
PreviewMode previewMode = PREVIEW_OFF;
uint8_t  previewDigit = 0, previewSegment = 0;
uint16_t previewGroup = 0;        // /probe allows up to 511 groups; a uint8_t truncated them
uint32_t previewTouchedMs = 0;
bool     previewDirty     = false;

// A wiring preview LATCHES. The first cut expired after 1500 ms, which made the
// lit segment a blip you could easily be looking away from -- and you are meant
// to be looking at the bench, not the screen. It now stays lit until you answer,
// which is also the only way "click the segment that lit up" is a fair question.
//
// ⚠ Latching means something must un-latch it if the tab simply goes away, or a
// closed browser would leave the clock suspended forever. Five minutes of
// silence from the page releases the panel back to the clock.
static constexpr uint32_t PREVIEW_IDLE_MS = 300000UL;

// The one predicate every painter consults. Guarding each call site instead was
// the original bug: showTime() ran unconditionally at the end of loop() and
// repainted over the preview five times a second.
inline bool wiringOwnsPanel() { return previewMode != PREVIEW_OFF; }

// ---------------------------------------------------------------- display
void renderDigit(uint8_t pos, uint8_t segmentMask, CRGB colour) {
  renderSegments(leds, cfg, pos, segmentMask, colour);
}

void showSegment(uint8_t pos, uint8_t seg, CRGB colour) {
  renderDigit(pos, (uint8_t)(1U << seg), colour);
}

void setActiveLedCount(uint16_t oldCount = 0) {
  if (!ledController) return;
  uint16_t nextCount = activeLedCount(cfg);
  uint16_t clearCount = max(oldCount, nextCount);
  fill_solid(leds, clearCount, CRGB::Black);
  ledController->setLeds(leds, clearCount);
  FastLED.show();
  ledController->setLeds(leds, nextCount);
}

void cancelPreview() {
  previewMode = PREVIEW_OFF;
  previewDirty = false;
}

bool serviceWiringPreview() {
  if (previewMode == PREVIEW_OFF) return false;
  if (millis() - previewTouchedMs > PREVIEW_IDLE_MS) {   // tab went away
    cancelPreview();
    return false;
  }
  // Latched and unchanged: the panel is already correct, so repainting it at
  // loop speed would only burn SPI time and risk visible flicker.
  if (!previewDirty) return true;
  previewDirty = false;

  clearDisplay(leds, cfg);
  if (previewMode == PREVIEW_IDENTIFY) {
    showSegment(previewDigit, previewSegment, CRGB::White);
  } else if (previewMode == PREVIEW_PROBE) {
    uint16_t base = previewGroup * cfg.ledsPerSeg;
    for (uint8_t led = 0; led < cfg.ledsPerSeg; led++) {
      uint16_t idx = base + led;
      if (idx < activeLedCount(cfg)) leds[idx] = CRGB::White;
    }
  } else {
    cancelPreview();
    return false;
  }
  FastLED.show();
  return true;
}

// Service the web server and the wiring preview's idle watchdog. Called from
// every branch of loop() and from inside the blocking ticker animations, so a
// closed tab always releases the panel.
bool pumpWeb() {
  server.handleClient();
  return serviceWiringPreview();
}

// ⚠ Only these render: 0-9 A-F H J L N O P R U Y - _ and space. Anything else
// comes back as a blank digit, silently. "Sync", "boot" and "oTA" were all early
// drafts with a letter the library cannot draw (S, t, T), so they showed holes.
// Any digit that would blank lights its DP as a tell.
void showWord(const char* s) {
  clearDisplay(leds, cfg);
  for (uint8_t i = 0; i < cfg.digits && s[i]; i++) {
    renderChar(leds, cfg, i, s[i], CRGB(cfg.r, cfg.g, cfg.b));
    if (String7Segment::getPattern(s[i]) == 0 && s[i] != ' ')
      renderDecimalPoint(leds, cfg, i, CRGB(cfg.r, cfg.g, cfg.b));   // "this character does not exist"
  }
  FastLED.show();
}

// The firmware version, "1.2.3" as digits with decimal points, for a moment at
// boot. It is the cheapest proof that an update landed: the clock says what it
// is running before it does anything else. Each '.' rides on the digit before
// it, so N.N.N needs three digits; Release.ts refuses a version whose digits do
// not fit a four-digit panel. On a smaller panel it is skipped, not truncated.
bool showVersion() {
  const char* v = FW_VERSION;
  uint8_t digitsNeeded = 0;
  for (const char* p = v; *p; p++) if (*p >= '0' && *p <= '9') digitsNeeded++;
  if (digitsNeeded == 0 || digitsNeeded > cfg.digits) return false;
  clearDisplay(leds, cfg);
  CRGB c(cfg.r, cfg.g, cfg.b);
  int8_t pos = -1;
  for (const char* p = v; *p; p++) {
    if (*p >= '0' && *p <= '9') renderChar(leds, cfg, ++pos, *p, c);
    else if (*p == '.' && pos >= 0) renderDecimalPoint(leds, cfg, pos, c);
  }
  FastLED.show();
  return true;
}

// Busy indicator for waits. A static word during a 15 s connect looks like a
// hung board; a moving one does not. No letters, so no character-set risk.
void showSpin(uint8_t step) {
  static const uint8_t SPIN_SEGMENTS[] = { SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F };
  clearDisplay(leds, cfg);
  for (uint8_t d = 0; d < cfg.digits; d++)
    renderDigit(d, SPIN_SEGMENTS[(step + d) % 6], CRGB(cfg.r, cfg.g, cfg.b));
  FastLED.show();
}

void showTime() {
  // The single gate. showTime() is reached from several places and guarding
  // each of them was how the preview got painted over in the first place. One
  // refusal here covers every caller, including any added later.
  if (wiringOwnsPanel()) return;

  struct tm t;
  if (!getLocalTime(&t, 50)) return;

  int hh = t.tm_hour, mm = t.tm_min, ss = t.tm_sec;
  if (cfg.hour12) { hh = hh % 12; if (hh == 0) hh = 12; }
  uint32_t ms = millis();

  // The face follows the panel. Six or more digits show HH:MM:SS, four or five
  // show HH:MM, fewer show the hour. The 1.1.0 face was hard-wired to four, so a
  // six-digit build learned on /wiring showed two permanently dark digits.
  char face[9];
  if (cfg.digits >= 6)      snprintf(face, sizeof face, "%02d%02d%02d", hh, mm, ss);
  else if (cfg.digits >= 4) snprintf(face, sizeof face, "%02d%02d", hh, mm);
  else if (cfg.digits >= 2) snprintf(face, sizeof face, "%02d", hh);
  else                      snprintf(face, sizeof face, "%d", hh % 10);   // one digit: the hour's units
  uint8_t shown = min<uint8_t>(cfg.digits, (uint8_t)strlen(face));

  clearDisplay(leds, cfg);
  // Foreground is set per digit rather than once: that is what lets SPECTRUM
  // spread across the display, and the modes that ignore position simply
  // return the same colour every time.
  for (uint8_t i = 0; i < shown; i++) {
    if (i == 0 && cfg.hour12 && hh < 10) continue;   // blank, not zero-padded
    CRGB c = colourFor(cfg, i, t, ms);
    renderDigit(i, String7Segment::getPattern(face[i]), c);
  }

  // No colon on this panel, so digit 1's decimal point stands in — blinking on
  // the second by default, which also makes "still alive" visible for free. A
  // six-digit face gets the second colon on digit 3.
  bool dp = cfg.colon == COLON_ON  ? true
          : cfg.colon == COLON_OFF ? false
          : (t.tm_sec % 2 == 0);
  if (dp && shown >= 4) {
    renderDecimalPoint(leds, cfg, 1, colourFor(cfg, 1, t, ms));
    if (shown >= 6) renderDecimalPoint(leds, cfg, 3, colourFor(cfg, 3, t, ms));
  }

  // Seconds overlay goes on LAST, straight into the pixel buffer, because it
  // has to know which segments the digits actually lit — that masking is the
  // whole idea. Doing it through the display API would mean re-deriving the
  // patterns the library just finished drawing.
  applySeconds(leds, activeLedCount(cfg), secondsNow(t), cfg, colourFor(cfg, 0, t, ms), shown);

  // Offline lights the LAST lit digit's dot as well: the time is the last known
  // good one and may be drifting. Blanking the display instead would be worse —
  // a clock that goes dark every time the router hiccups is useless.
  if (WiFi.status() != WL_CONNECTED && shown > 0)
    renderDecimalPoint(leds, cfg, shown - 1, colourFor(cfg, shown - 1, t, ms));
  // Update available: the same dot FLASHES — 0.4 s in every second, clearly not
  // the colon, which is on for a whole second at a time. Offline (steady) wins,
  // since an offline clock cannot act on an update anyway.
  else if (updAvail && cfg.upd != UPD_OFF && shown > 0 && (ms % 1000) < 400)
    renderDecimalPoint(leds, cfg, shown - 1, colourFor(cfg, shown - 1, t, ms));
  FastLED.show();
}

// A word that must be SEEN — "Err" after a failed update — would otherwise be
// repainted over by the next frame 200 ms later. Hold the frame painter off.
uint32_t holdUntilMs = 0;
void holdWord(const char* w) {
  showWord(w);
  holdUntilMs = millis() + 3000;
}

// One frame every FRAME_MS, whatever loop() is doing. Every branch calls this so
// the display never depends on which state the network machine is in.
void paintFrame() {
  uint32_t now = millis();
  if (now - lastPaintMs < FRAME_MS) return;
  if (holdUntilMs && (int32_t)(holdUntilMs - now) > 0) return;
  holdUntilMs = 0;
  lastPaintMs = now;
  if (timeValid) showTime();
  else if (wiringOwnsPanel()) return;
  else if (portalUp) showWord("AP");   // the one hint to go and find the setup network
  else { showSpin(spinStep); spinStep = (spinStep + 1) % 6; }
}

// ---------------------------------------------------------------- OTA rollback
// The bootloader in this core supports rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE),
// but initArduino() marks a freshly flashed image valid before setup() even runs,
// so a build that crashes later would boot-loop with no way back. Returning true
// here keeps that decision for us: the image is confirmed only after it has run
// for IMAGE_CONFIRM_MS AND is reachable — on the network, or with its own setup
// AP up. An image that neither crashes nor can be reached must not be confirmed,
// because a power cycle would then be the only way back to the one that worked.
// The router being down is not held against it: the portal counts as reachable.
extern "C" bool verifyRollbackLater() { return true; }

const char* otaStateName() {
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return "unknown";
  switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";   // factory image, or rollback not tracked
  }
}

bool imagePending() {
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}

// A restart the user asks for from the page is proof the image is reachable and
// working — so it confirms the image first. Without this, "Restart now to use
// GPIO 5" clicked 30 s after an update would roll the firmware back.
void confirmBeforeRestart() {
  if (imagePending() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
    Serial.println("[OTA] image confirmed by a user-requested restart");
}

void confirmImage() {
  static bool done = false;
  if (done || millis() < IMAGE_CONFIRM_MS) return;
  if (WiFi.status() != WL_CONNECTED && !portalUp) return;   // not reachable yet; keep waiting
  if (!imagePending()) { done = true; return; }             // factory image, or already confirmed
  // done only on success: a failed otadata write is retried next pass, not logged as a win
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    done = true;
    Serial.println("[OTA] image confirmed: ran 60 s and is reachable; rollback cancelled");
  } else {
    Serial.printf("[OTA] confirm failed: %s (will retry)\n", esp_err_to_name(err));
  }
}

// ---------------------------------------------------------------- credentials
bool hasCreds() {
  prefs.begin("wifi", true);
  bool have = prefs.getString("ssid", "").length() > 0;
  prefs.end();
  return have;
}

// ---------------------------------------------------------------- portal
// The credentials form posts with fetch() so it can carry the X-7seg header —
// /save is a write like any other. Captive-portal mini-browsers run this fine.
const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>
<title>mini7seg clock</title>
<style>body{font:16px system-ui;margin:2rem auto;max-width:22rem;padding:0 1rem}
input,button{font:inherit;width:100%;padding:.6rem;margin:.3rem 0;box-sizing:border-box}
button{background:#111;color:#fff;border:0;border-radius:.3rem}#m{color:#b00;font-size:.9rem;min-height:1.2em}</style>
<h2>%NAME% clock</h2><form id=f>
<input id=s name=ssid placeholder='WiFi network' maxlength=32 required autocapitalize=off autocorrect=off spellcheck=false>
<input id=p name=pass type=password placeholder='Password (blank for an open network)' maxlength=63>
<button>Save and restart</button></form><p id=m></p>
<script>
document.getElementById('f').onsubmit=e=>{e.preventDefault();const m=document.getElementById('m');m.textContent='Saving...';
fetch('/save?'+new URLSearchParams({ssid:document.getElementById('s').value,pass:document.getElementById('p').value}),
 {method:'POST',headers:{'X-7seg':'1'}}).then(r=>r.json()).then(j=>{m.style.color=j.ok?'#080':'#b00';
 m.textContent=j.ok?'Saved. Restarting - join your network and open %NAME%.local':('Not saved: '+(j.error||'unknown'))})
 .catch(()=>{m.textContent='The clock did not answer. Try again.'})};
</script>)HTML";

void handleRoot() {
  // One route, two pages: the credentials form while the portal is up, the
  // settings page once we are on a real network. The portal page names the
  // host the clock will come back under, which is a setting now.
  if (portalUp) {
    String page = FPSTR(PORTAL_HTML);
    page.replace("%NAME%", hostName);
    server.send(200, "text/html; charset=utf-8", page);
  } else {
    server.send_P(200, "text/html; charset=utf-8", SETTINGS_HTML);
  }
}

String   latestTag;       // "" until a check has run; normalised (no leading v)
String   latestRawTag;    // exactly as GitHub spells it, for the download URL
int      lastCheckCode = 0;
String   updErr;          // last self-update failure, for the page to show

void sendState() {
  struct tm t;
  char clockStr[16] = "--:--";
  if (timeValid && getLocalTime(&t, 50))
    snprintf(clockStr, sizeof clockStr, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);

  // Built as a String, not a fixed snprintf buffer: city and the update error
  // are external text of unbounded length, and a truncated or unescaped value
  // used to hand the page invalid JSON, which killed the whole settings page.
  String o;
  o.reserve(1000);
  o += "{\"hue\":" + String(cfg.hue) + ",\"spread\":" + String(cfg.spread) + ",\"env\":" + String(cfg.env);
  o += ",\"secpath\":" + String(cfg.secpath) + ",\"sectrail\":" + String(cfg.sectrail);
  o += ",\"r\":" + String(cfg.r) + ",\"g\":" + String(cfg.g) + ",\"b\":" + String(cfg.b) + ",\"bri\":" + String(cfg.brightness);
  o += ",\"rst\":\"" + String(resetReasonName()) + "\",\"boots\":" + String(bootCount) + ",\"brownouts\":" + String(brownoutCount);
  o += ",\"upMin\":" + String(millis() / 60000UL);
  // What FastLED BELIEVES the panel draws with the current buffer at full
  // brightness. FastLED models 5050-class parts, so on a 2020 panel the number
  // is generous.
  o += ",\"estMw\":" + String(calculate_unscaled_power_mW(leds, activeLedCount(cfg)));
  o += ",\"diedAt\":" + String(lastStressTry);
  o += ",\"h12\":" + String(cfg.hour12 ? "true" : "false") + ",\"colon\":" + String(cfg.colon) + ",\"speed\":" + String(cfg.speed);
  o += ",\"tick\":" + String(cfg.tickMins) + ",\"cards\":" + String(cfg.cards);
  o += ",\"digits\":" + String(cfg.digits) + ",\"ledsPerSeg\":" + String(cfg.ledsPerSeg) + ",\"dpMask\":" + String(cfg.dpMask);
  o += ",\"fw\":\"" FW_VERSION "\",\"pin\":" + String(cfg.dataPin) + ",\"bootPin\":" + String(bootPin) + ",\"pins\":[" + pinListJson() + "]";
  o += ",\"name\":\"" + jsonEscape(cfg.name) + "\",\"tz\":\"" + jsonEscape(cfg.tz) + "\"";
  o += ",\"btc\":" + String(btc.usd);
  // null, not 0, when there is no reading: 0 F is a real temperature.
  o += ",\"tempF\":" + (btc.wxOk ? String(btc.tempF, 1) : String("null"));
  o += ",\"city\":\"" + jsonEscape(cfg.city) + "\"";
  o += ",\"time\":\"" + String(clockStr) + "\",\"timeValid\":" + String(timeValid ? "true" : "false");
  o += ",\"syncMin\":" + (lastRealSyncMs ? String((millis() - lastRealSyncMs) / 60000UL) : String("null"));
  o += ",\"online\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  o += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"host\":\"" + String(hostName) + "\"";
  o += ",\"updErr\":\"" + jsonEscape(updErr) + "\",\"checkCode\":" + String(lastCheckCode);
  o += ",\"upd\":" + String(cfg.upd) + ",\"updAvail\":" + String(updAvail ? "true" : "false");
  o += ",\"updLatest\":\"" + jsonEscape(latestTag) + "\"";
  o += ",\"updCheckMin\":" + (lastCheckMs ? String((millis() - lastCheckMs) / 60000UL) : String("null"));
  // Rollback state of the running image and the size of the slot the next update
  // lands in: the two facts a release test has to read off the device itself.
  const esp_partition_t* nextSlot = esp_ota_get_next_update_partition(NULL);
  o += ",\"otaState\":\"" + String(otaStateName()) + "\",\"otaSlot\":" + String(nextSlot ? nextSlot->size : 0);
  o += ",\"heap\":" + String(ESP.getFreeHeap()) + "}";   // TLS needs ~40 KB; a bench number worth having
  server.send(200, "application/json", o);
}

void handleSet() {
  if (!requireWrite()) return;
  static const char* const allowed[] = {
    "hue", "spread", "env", "secpath", "sectrail", "r", "g", "b", "bri",
    "colon", "speed", "tick", "cards", "pin", "h12", "tz", "name", "upd"
  };
  if (!rejectUnexpectedArgs(allowed, sizeof(allowed) / sizeof(allowed[0]))) return;

  // Validate into a copy, apply only if everything passed. Writing into cfg
  // field by field meant a request rejected on its third argument had already
  // changed the first two.
  Settings next = cfg;
  if (!readOptionalU8("hue",    next.hue,        0, HUE_COUNT - 1)) return;
  if (!readOptionalU8("spread", next.spread,     0, 64)) return;
  if (!readOptionalU8("env",    next.env,        0, ENV_COUNT - 1)) return;
  if (!readOptionalU8("secpath",  next.secpath,  0, PATH_COUNT - 1)) return;
  if (!readOptionalU8("sectrail", next.sectrail, 0, TRAIL_COUNT - 1)) return;
  if (!readOptionalU8("r",      next.r,          0, 255)) return;
  if (!readOptionalU8("g",      next.g,          0, 255)) return;
  if (!readOptionalU8("b",      next.b,          0, 255)) return;
  if (!readOptionalU8("bri",    next.brightness, 5, 255)) return;
  if (!readOptionalU8("colon",  next.colon,      0, COLON_OFF)) return;
  if (!readOptionalU8("speed",  next.speed,      1, 20)) return;
  if (!readOptionalU8("tick",   next.tickMins,   0, 60)) return;
  if (!readOptionalU8("cards",  next.cards,      0, 3)) return;
  if (!readOptionalU8("upd",    next.upd,        0, UPD_COUNT - 1)) return;
  // Validated against the allow-list, not just a range: an arbitrary pin would
  // fall through to the default and silently keep using 4, which looks like the
  // setting simply does not work.
  if (server.hasArg("pin")) {
    long want;
    if (!parseLongStrict(server.arg("pin"), want)) { sendJsonError(400, "pin must be an integer"); return; }
    bool ok = false;
    for (uint8_t p : PIN_LIST) if (p == want) { ok = true; break; }
    if (!ok) { sendJsonError(400, "pin is not in this chip's allow-list"); return; }
    next.dataPin = (uint8_t)want;
  }
  if (server.hasArg("h12")) {
    long h12;
    if (!parseLongStrict(server.arg("h12"), h12)) { sendJsonError(400, "h12 must be an integer"); return; }
    if (h12 < 0 || h12 > 1) { sendJsonError(400, "h12 must be 0..1"); return; }
    next.hour12 = h12 != 0;
  }
  if (server.hasArg("tz")) {
    String tz = server.arg("tz");
    if (!validTz(tz.c_str())) { sendJsonError(400, "tz must be a POSIX rule like PST8PDT,M3.2.0/2,M11.1.0/2 (1..47 printable chars, with a digit)"); return; }
    setField(next.tz, sizeof next.tz, tz);
  }
  if (server.hasArg("name")) {
    String name = server.arg("name");
    name.toLowerCase();
    if (!validName(name.c_str())) { sendJsonError(400, "name must be 1..24 of a-z 0-9 - and not start or end with -"); return; }
    setField(next.name, sizeof next.name, name);
  }

  // Flash first, then RAM: a failed write must change nothing, not "everything
  // until the next reboot".
  if (!saveSettings(next)) { sendJsonError(500, "could not write settings to flash; nothing changed"); return; }
  bool tzChanged = strcmp(next.tz, cfg.tz) != 0;
  cfg = next;
  FastLED.setBrightness(cfg.brightness);
  if (tzChanged) { setenv("TZ", cfg.tz, 1); tzset(); }   // live; no resync needed, the rule is applied on read
  sendState();
}

String geometryJson() {
  String out;
  out.reserve(380);
  out += "{\"ok\":true,\"digits\":";     // ok:true so the page can tell a save from a rejection
  out += cfg.digits;
  out += ",\"ledsPerSeg\":";
  out += cfg.ledsPerSeg;
  out += ",\"dpMask\":";
  out += cfg.dpMask;
  out += ",\"stripLen\":";
  out += cfg.stripLen;
  out += ",\"ledCount\":";
  out += activeLedCount(cfg);
  out += ",\"segBase\":[";
  for (uint8_t d = 0; d < MAX_DIGITS; d++) {
    if (d) out += ',';
    out += '[';
    for (uint8_t seg = 0; seg < SEGMENT_COUNT; seg++) {
      if (seg) out += ',';
      out += cfg.segBase[d][seg];
    }
    out += ']';
  }
  out += "]}";
  return out;
}

void sendGeometry() {
  server.send(200, "application/json", geometryJson());
}

bool parseSegBase(Settings& next, String& problem) {
  // Accepts EITHER next.digits*8 values or the full MAX_DIGITS*8, because the
  // rows past next.digits have exactly one legal value -- SEGMENT_ABSENT -- and
  // making a client transmit 32 copies of a constant is busywork it can only get
  // wrong. Short form is filled in here; long form is still validated in full so
  // an explicit table that disagrees is caught rather than quietly overwritten.
  const uint8_t wantRows = next.digits;
  String raw = server.arg("segBase");
  const char* p = raw.c_str();

  for (uint8_t d = 0; d < MAX_DIGITS; d++)
    for (uint8_t seg = 0; seg < SEGMENT_COUNT; seg++)
      next.segBase[d][seg] = SEGMENT_ABSENT;

  uint16_t count = 0;
  for (uint8_t d = 0; d < MAX_DIGITS; d++) {
    for (uint8_t seg = 0; seg < SEGMENT_COUNT; seg++) {
      while (*p == ' ' || *p == '\t') p++;
      if (!*p) {
        // Running out exactly at the end of the active rows is the short form.
        if (count == (uint16_t)wantRows * SEGMENT_COUNT) return true;
        problem = String("segBase needs ") + (wantRows * SEGMENT_COUNT) + " or " +
                  (MAX_DIGITS * SEGMENT_COUNT) + " values, got " + count;
        return false;
      }
      if (*p == '-') {
        problem = String("segBase values must be 0..") + SEGMENT_BASE_MAX + " or " + SEGMENT_ABSENT + " for absent";
        return false;
      }
      char* end;
      long v = strtol(p, &end, 10);
      if (end == p) {
        problem = String("segBase[") + d + "][" + seg + "] must be an integer";
        return false;
      }
      while (*end == ' ' || *end == '\t') end++;
      if (v == (long)SEGMENT_ABSENT) {
        next.segBase[d][seg] = SEGMENT_ABSENT;
      } else if (v > SEGMENT_BASE_MAX) {
        problem = String("segBase[") + d + "][" + seg + "] must be 0.." + SEGMENT_BASE_MAX + " or " + SEGMENT_ABSENT + " for absent";
        return false;
      } else {
        next.segBase[d][seg] = (uint16_t)v;
      }
      count++;
      p = end;
      if (*p == ',') p++;
    }
  }
  while (*p == ' ' || *p == '\t' || *p == ',') p++;
  if (*p) {
    problem = String("segBase has more than ") + (MAX_DIGITS * SEGMENT_COUNT) + " values";
    return false;
  }
  return true;
}

bool applyGeometry(const Settings& next) {
  uint16_t oldCount = activeLedCount(cfg);
  cfg.digits = next.digits;
  cfg.ledsPerSeg = next.ledsPerSeg;
  cfg.dpMask = next.dpMask;
  cfg.stripLen = next.stripLen;
  memcpy(cfg.segBase, next.segBase, sizeof(cfg.segBase));
  cancelPreview();
  setActiveLedCount(oldCount);
  return saveSettings(cfg);
}

void handleGeometry() {
  if (!rejectNoArgs()) return;
  sendGeometry();
}

void handleSetGeometry() {
  if (!requireWrite()) return;
  static const char* const allowed[] = { "digits", "ledsPerSeg", "dpMask", "segBase", "stripLen", "reset" };
  if (!rejectUnexpectedArgs(allowed, sizeof(allowed) / sizeof(allowed[0]))) return;

  Settings next = cfg;
  if (server.hasArg("reset")) {
    long reset;
    if (!parseLongStrict(server.arg("reset"), reset)) { sendJsonError(400, "reset must be an integer"); return; }
    if (reset != 1) { sendJsonError(400, "reset must be 1"); return; }
    if (server.args() != 1) { sendJsonError(400, "reset cannot be combined with other geometry arguments"); return; }
    next.digits = DEFAULT_DIGITS;
    next.ledsPerSeg = DEFAULT_LEDS_PER_SEG;
    next.dpMask = DEFAULT_DP_MASK;
    next.stripLen = 0;
    geometryReset(next);
    if (!applyGeometry(next)) { sendJsonError(500, "could not write geometry to flash"); return; }
    sendGeometry();
    return;
  }

  long v;
  if (!readLongArg("digits", 1, MAX_DIGITS, v)) return;
  next.digits = (uint8_t)v;
  if (!readLongArg("ledsPerSeg", 1, MAX_LEDS_PER_SEG, v)) return;
  next.ledsPerSeg = (uint8_t)v;
  if (!readLongArg("dpMask", 0, (1U << MAX_DIGITS) - 1U, v)) return;
  next.dpMask = (uint16_t)v;
  // 0 is legal and means derive. Anything shorter than the segments need is
  // clamped up rather than rejected, since it is the segments that are load
  // bearing -- a too-short strip length would orphan LEDs that are genuinely in use.
  if (server.hasArg("stripLen")) {
    if (!readLongArg("stripLen", 0, MAX_LEDS, v)) return;
    next.stripLen = (uint16_t)v;
  }

  if (server.hasArg("segBase")) {
    String problem;
    if (!parseSegBase(next, problem)) { sendJsonError(400, problem); return; }
  } else {
    char err[96];
    if (packedGeometryProblem(next, err, sizeof err)) { sendJsonError(400, err); return; }
    geometryReset(next);
  }

  char err[96];
  if (geometryProblem(next, err, sizeof err)) { sendJsonError(400, err); return; }
  if (!applyGeometry(next)) { sendJsonError(500, "could not write geometry to flash"); return; }
  sendGeometry();
}

void handleIdentify() {
  if (!requireWrite()) return;
  static const char* const allowed[] = { "d", "s" };
  if (!rejectUnexpectedArgs(allowed, sizeof(allowed) / sizeof(allowed[0]))) return;
  long d, seg;
  if (!readLongArg("d", 0, cfg.digits - 1, d)) return;
  if (!readLongArg("s", 0, SEGMENT_COUNT - 1, seg)) return;
  if (!segmentWritable(cfg, (uint8_t)d, (uint8_t)seg)) {
    sendJsonError(400, String("segment d=") + d + " s=" + seg + " is absent");
    return;
  }

  previewMode = PREVIEW_IDENTIFY;
  previewDirty = true;
  previewDigit = (uint8_t)d;
  previewSegment = (uint8_t)seg;
  previewTouchedMs = millis();
  serviceWiringPreview();
  server.send(200, "application/json", "{\"ok\":true,\"mode\":\"identify\"}");
}

void handleProbe() {
  if (!requireWrite()) return;
  static const char* const allowed[] = { "i" };
  if (!rejectUnexpectedArgs(allowed, sizeof(allowed) / sizeof(allowed[0]))) return;
  long i;
  // Round UP: a strip whose length is not a multiple of ledsPerSeg has a partial
  // last group, and the wizard must be able to light it too.
  long groups = ((long)activeLedCount(cfg) + cfg.ledsPerSeg - 1) / cfg.ledsPerSeg;
  if (!readLongArg("i", -1, groups - 1, i)) return;
  if (i < 0) {
    cancelPreview();
    clearDisplay(leds, cfg);
    FastLED.show();
    server.send(200, "application/json", "{\"ok\":true,\"mode\":\"off\"}");
    return;
  }

  previewMode = PREVIEW_PROBE;
  previewDirty = true;
  previewGroup = (uint16_t)i;
  previewTouchedMs = millis();
  serviceWiringPreview();
  server.send(200, "application/json", "{\"ok\":true,\"mode\":\"probe\"}");
}

void handleSave() {
  if (!requireWrite()) return;
  static const char* const allowed[] = { "ssid", "pass" };
  if (!rejectUnexpectedArgs(allowed, sizeof(allowed) / sizeof(allowed[0]))) return;
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  // 802.11 caps these. Over-length stores fine and then never associates, which
  // looks like a wrong password forever.
  if (ssid.isEmpty()) { sendJsonError(400, "ssid is required"); return; }
  if (ssid.length() > SSID_MAX) { sendJsonError(400, "ssid must be 1..32 chars"); return; }
  if (pass.length() > PASS_MAX) { sendJsonError(400, "pass must be at most 63 chars"); return; }

  // Every one of these can fail — NVS full, partition corrupt — and the old
  // code discarded all three return values and told the user "Saved". The
  // device then rebooted, found nothing, and returned to the portal in a loop
  // that reported success on every pass.
  bool ok = prefs.begin("wifi", false);
  if (ok) ok = prefs.putString("ssid", ssid) > 0;
  if (ok) {
    // An empty password means an open network. It used to leave the previous
    // password in place, so the clock could never join an open network after a
    // secured one.
    if (pass.isEmpty()) prefs.remove("pass");
    else ok = prefs.putString("pass", pass) > 0;
  }
  prefs.end();

  if (!ok) { sendJsonError(500, "could not write wifi settings to flash"); return; }

  server.send(200, "application/json", "{\"ok\":true,\"host\":\"" + String(hostName) + "\"}");
  delay(600);
  confirmBeforeRestart();
  ESP.restart();
}

// The web server runs in BOTH modes — portal and normal — so there is exactly
// one place routes are registered and no window where the device is on the
// network but unreachable.
// Browser upload, the way WLED does it: POST a .bin straight at the device.
// ArduinoOTA (already armed) needs espota and a toolchain; this needs a file
// picker. Different jobs — one is for me mid-iteration, the other is for you
// standing in front of the clock with a phone.
const char UPDATE_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>update</title>
<style>body{margin:0;background:#0d0f13;color:#e8eaed;font:15px system-ui}
.w{max-width:26rem;margin:0 auto;padding:1.5rem 1.1rem}h1{font-size:1.1rem}
input,button{font:inherit;width:100%;padding:.6rem;margin:.35rem 0;box-sizing:border-box}
button{background:#00a0ff;color:#04121d;border:0;border-radius:.4rem;font-weight:600}
progress{width:100%;height:.5rem}a{color:#00a0ff}
.n{color:#8b929c;font-size:.8rem;margin-top:1rem}</style>
<div class=w><h1>Firmware update</h1>
<form id=f method=POST action=/update enctype=multipart/form-data>
<input type=file name=u accept=".bin" required><button>Upload &amp; restart</button></form>
<progress id=p value=0 max=100 hidden></progress><p id=s class=n></p>
<p class=n>Use <b>firmware.bin</b> from a GitHub release of mcyork/7segclock, or from
.pio/build/c3supermini/ if you built it yourself &mdash; not firmware.factory.bin,
which includes the bootloader and is for first flash over USB.</p>
<p class=n><a href="/">&larr; back to settings</a></p></div>
<script>
const f=document.getElementById('f'),p=document.getElementById('p'),s=document.getElementById('s');
f.onsubmit=e=>{e.preventDefault();const x=new XMLHttpRequest(),d=new FormData(f);
 p.hidden=false;
 x.upload.onprogress=v=>{p.value=v.loaded/v.total*100;s.textContent='Uploading '+p.value.toFixed(0)+'%'};
 x.onload=()=>{s.textContent=x.status==200?'Done - restarting. This page will not respond for ~10s.':'Failed: '+x.responseText};
 x.onerror=()=>{s.textContent='Connection lost during upload.'};
 x.open('POST','/update');x.setRequestHeader('X-7seg','1');x.send(d);};
</script>)HTML";

bool uploadOk = false;        // set by the END branch; the completion handler trusts only this
bool uploadRejected = false;  // header missing at START: nothing is written

void handleUpdateUpload() {
  // WebServer also routes a NON-multipart POST here (its "raw body" path), and in
  // that case server.upload() is a null reference — touching it panicked the
  // clock, so `curl -X POST /update` from any LAN host, or a text/plain form on
  // any web page, was a remote reboot. Refuse before reading anything.
  if (!server.header("Content-Type").startsWith("multipart/")) { uploadOk = false; uploadRejected = true; return; }
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadOk = false;
    uploadRejected = server.header("X-7seg") != "1" || (!portalUp && !hostIsOurs(server.hostHeader()));
    if (uploadRejected) { Serial.println("[UPD] upload refused: header or Host"); return; }
    Serial.printf("[UPD] %s\n", up.filename.c_str());
    showWord("UPd");
    // Someone uploading from the page has reached it: confirm a pending image
    // first, or Update.begin() refuses with ESP_ERR_OTA_ROLLBACK_INVALID_STATE.
    confirmBeforeRestart();
    // UPDATE_SIZE_UNKNOWN: the browser does not tell us the length up front, so
    // let the Update library size it against the free OTA partition.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadRejected) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadRejected) return;
    uploadOk = Update.end(true);
    if (uploadOk) { Serial.printf("[UPD] ok, %u bytes\n", (unsigned)up.totalSize); showWord("donE"); }
    else { Update.printError(Serial); holdWord("Err"); }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    // A dropped connection otherwise left Update "already running" until the
    // next reboot, and every later upload failed at begin().
    Update.abort();
    holdWord("Err");
    Serial.println("[UPD] upload aborted");
  }
}

/** Compare dotted versions numerically. "1.10.0" is NEWER than "1.9.0", which a
 *  string compare gets exactly backwards — the usual way this goes wrong. */
inline bool isNewer(const String& a, const String& b) {
  int ai = 0, bi = 0;
  for (uint8_t part = 0; part < 3; part++) {
    long av = a.substring(ai).toInt(), bv = b.substring(bi).toInt();
    if (av != bv) return av > bv;
    ai = a.indexOf('.', ai) + 1; bi = b.indexOf('.', bi) + 1;
    if (ai <= 0 || bi <= 0) break;
  }
  return false;
}

/** Ask GitHub for the newest release tag. Keyless: public repos allow 60
 *  unauthenticated calls an hour per IP, and this runs only when someone presses
 *  Check for updates — there is no automatic check. The HTTP status is kept so the
 *  page can say "no release yet" (404) or "rate limited" (403) instead of a
 *  blanket "unreachable". */
/** Exactly N.N.N, digits only. toInt() would accept "1.3.0-test" as 1.3.0 and then
 *  put the whole tag into a URL; anything that is not a plain version is refused. */
bool plainVersion(const String& v) {
  uint8_t parts = 1, digits = 0;
  for (uint16_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c >= '0' && c <= '9') { if (++digits > 4) return false; }
    else if (c == '.' && digits) { if (++parts > 3) return false; digits = 0; }
    else return false;
  }
  return parts == 3 && digits > 0;
}

/** Follow redirects by hand with the bundle client, HEAD only, and return the
 *  first URL we could not (or need not) go past. github.com's 302 to the CDN is
 *  the case that matters: the CDN host may not verify against the bundle, and
 *  that is decided by the download attempts, not here. */
String resolveRedirect(const String& url) {
  String cur = url;
  for (uint8_t hop = 0; hop < 4; hop++) {
    WiFiClientSecure tls;
    trustRootBundle(tls);
    HTTPClient http;
    if (!http.begin(tls, cur)) return cur;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    int code = http.sendRequest("HEAD");
    String loc = http.getLocation();
    http.end();
    if (code >= 300 && code < 400 && loc.startsWith("https://")) { cur = loc; continue; }
    return cur;                    // 200, an error, or a relative/odd Location: let the download decide
  }
  return cur;
}

bool checkUpdate() {
  String b = httpGet(GH_LATEST, true, &lastCheckCode);
  int k = b.indexOf("\"tag_name\":\"");
  if (k < 0) return false;
  int e = b.indexOf('"', k + 12);
  if (e < 0) return false;
  String raw = b.substring(k + 12, e);
  String tag = raw.startsWith("v") ? raw.substring(1) : raw;
  if (!plainVersion(tag)) { Serial.printf("[UPD] refusing odd tag %s\n", raw.c_str()); lastCheckCode = 0; return false; }
  latestRawTag = raw;
  latestTag = tag;
  updAvail = isNewer(latestTag, FW_VERSION);   // drives the flashing dot and INSTALL mode
  lastCheckMs = millis();
  Serial.printf("[UPD] running %s, latest %s\n", FW_VERSION, latestTag.c_str());
  return true;
}

void handleFactoryReset() {
  if (!requireWrite()) return;
  if (!rejectNoArgs()) return;
  // Every namespace this firmware writes. The bootloader and the app slots are
  // untouched — this is "forget everything", not "unflash".
  Preferences p;
  for (const char* ns : { "disp", "wifi", "boots" }) {
    if (p.begin(ns, false)) { p.clear(); p.end(); }
  }
  server.send(200, "application/json", "{\"ok\":true}");
  delay(400);
  // ESP-IDF keeps its own copy of the last association in nvs.net80211; without
  // this the "forgotten" network is still there for the driver to auto-join.
  WiFi.disconnect(true, true);
  delay(100);
  confirmBeforeRestart();
  ESP.restart();
}

// Download and flash the release checkUpdate() last saw (latestRawTag). Shared by
// the Install button and INSTALL mode. Reboots on success; on failure records
// updErr for the page and holds "Err" on the panel.
void performUpdate() {
  showWord("UPd");
  updErr = "";
  // The asset URL is on github.com, whose chain the IDF bundle verifies, but it
  // answers with a redirect to GitHub's asset CDN — and THAT host chains to a
  // root the bundle in this core predates (Let's Encrypt "Root YR", found the
  // hard way on the first 1.2.0 self-update test). So: resolve the redirect on
  // the bundle client, then download with the bundle first and, if the CDN's
  // chain is not in it, with the anchors in roots.h. Never setInsecure().
  String asset = String(GH_DL) + latestRawTag + "/firmware.bin";  // the tag we just verified, not "latest"
  String target = resolveRedirect(asset);
  Serial.printf("[UPD] asset %s\n[UPD] -> %s\n", asset.c_str(), target.c_str());
  t_httpUpdate_return r = HTTP_UPDATE_FAILED;
  for (uint8_t attempt = 0; attempt < 2 && r != HTTP_UPDATE_OK; attempt++) {
    WiFiClientSecure tls;
    if (attempt == 0) trustRootBundle(tls); else tls.setCACert(EXTRA_ROOTS_PEM);
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true);
    r = httpUpdate.update(tls, target);
    if (r != HTTP_UPDATE_OK) {
      if (updErr.length()) updErr += "; ";
      updErr += String(attempt == 0 ? "bundle: " : "roots.h: ") + httpUpdate.getLastError() + " " + httpUpdate.getLastErrorString();
    }
  }
  if (r != HTTP_UPDATE_OK) {
    updErr = String(latestTag) + " " + updErr;
    Serial.printf("[UPD] failed %s\n", updErr.c_str());
    holdWord("Err");
  }
}

void startWeb() {
  if (webUp) return;
  webUp = true;
  static const char* HEADERS[] = { "X-7seg", "Content-Type" };
  server.collectHeaders(HEADERS, 2);        // or server.header() never sees them
  server.on("/",     handleRoot);
  server.on("/api",  sendState);
  server.on("/geometry", HTTP_GET, handleGeometry);
  // Writes: POST + X-7seg, enforced inside each handler by requireWrite(). The
  // method is also declared here so a GET falls through to the 404 handler.
  server.on("/set",         HTTP_POST, handleSet);
  server.on("/setgeometry", HTTP_POST, handleSetGeometry);
  server.on("/identify",    HTTP_POST, handleIdentify);
  server.on("/probe",       HTTP_POST, handleProbe);
  server.on("/save",        HTTP_POST, handleSave);
  server.on("/factoryreset", HTTP_POST, handleFactoryReset);
  server.on("/reboot", HTTP_POST, [] {
    if (!requireWrite()) return;
    if (!rejectNoArgs()) return;
    server.send(200, "application/json", "{\"ok\":true}");
    delay(200);
    confirmBeforeRestart();
    ESP.restart();
  });
  // Every LED, full white, for a few seconds: the worst case the panel can ever
  // present. Blocking on purpose -- loop() is not running inside a handler, so
  // the clock cannot repaint over it and no preview state machine is needed.
  // It does NOT pump the web server from inside itself any more: WebServer drops
  // the current client after 5 s of that and starts handling other requests
  // re-entrantly, so the hold is capped at 4 s and the loop just waits.
  //
  // Reports what FastLED BELIEVES it costs. If the board survives this, no
  // display content can brown it out; if it resets, the brownout counter in
  // /api will say so on the next boot, which is exactly the evidence a USB power
  // meter would have given.
  server.on("/stress", HTTP_POST, [] {
    if (!requireWrite()) return;
    long ms = 3000;
    if (server.hasArg("ms") && !parseLongStrict(server.arg("ms"), ms)) ms = 3000;
    if (ms < 100) ms = 100;
    if (ms > 4000) ms = 4000;
    // No limiter any more, so this is the genuine worst case. It ramps and
    // leaves a breadcrumb in NVS before each step: a step that browns the board
    // out cannot answer, but the next boot can say how far it got.
    uint16_t n = activeLedCount(cfg);
    uint8_t  savedBri = cfg.brightness;

    fill_solid(leds, n, CRGB::White);
    uint32_t mw  = calculate_unscaled_power_mW(leds, n);

    uint8_t reached = 0;
    Preferences sp;
    for (uint8_t step : { (uint8_t)64, (uint8_t)128, (uint8_t)192, (uint8_t)255 }) {
      if (sp.begin("boots", false)) { sp.putUChar("try", step); sp.end(); }
      FastLED.setBrightness(step);
      FastLED.show();
      delay((uint32_t)ms / 4);
      reached = step;
    }
    if (sp.begin("boots", false)) { sp.putUChar("try", 0); sp.end(); }   // survived
    lastStressTry = 0;

    FastLED.setBrightness(savedBri);
    clearDisplay(leds, cfg);
    FastLED.show();

    char b[200];
    snprintf(b, sizeof b,
      "{\"ok\":true,\"leds\":%u,\"heldMs\":%ld,"
      "\"modelMw\":%lu,\"modelMa\":%lu,"
      "\"reachedBrightness\":%u,\"survived\":true}",
      (unsigned)n, ms, (unsigned long)mw, (unsigned long)(mw / 5), (unsigned)reached);
    server.send(200, "application/json", b);
  });
  server.on("/wiring", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", GEOMETRY_HTML); });
  server.on("/update", HTTP_GET, [] {
    if (!rejectNoArgs()) return;
    server.send_P(200, "text/html; charset=utf-8", UPDATE_HTML);
  });
  // A read that costs a TLS round-trip and one of GitHub's 60 unauthenticated
  // calls an hour per address — so it is a write for guarding purposes: an <img>
  // on a stray page could otherwise stall the loop and burn the owner's budget.
  server.on("/checkupdate", HTTP_POST, [] {
    if (!requireWrite()) return;
    if (!rejectNoArgs()) return;
    bool ok = checkUpdate();
    String o = "{\"ok\":" + String(ok ? "true" : "false") + ",\"code\":" + String(lastCheckCode);
    o += ",\"current\":\"" FW_VERSION "\",\"latest\":\"" + jsonEscape(latestTag) + "\"";
    o += ",\"newer\":" + String((ok && isNewer(latestTag, FW_VERSION)) ? "true" : "false") + "}";
    server.send(200, "application/json", o);
  });
  server.on("/doupdate", HTTP_POST, [] {
    if (!requireWrite()) return;
    if (!rejectNoArgs()) return;
    // Re-ask GitHub now, and refuse unless the release is strictly newer. A
    // stale check, or a release that is not newer, must never flash — the 1.1.0
    // code would have happily downgraded a device to whatever `latest` was.
    if (!checkUpdate()) { sendJsonError(502, String("GitHub did not answer (HTTP ") + lastCheckCode + ")"); return; }
    if (!isNewer(latestTag, FW_VERSION)) { sendJsonError(409, "no newer release: latest is " + latestTag + ", running " FW_VERSION); return; }
    // Reply BEFORE flashing: the download takes ~20 s and the connection would
    // otherwise time out, leaving the page unable to say whether it worked.
    server.send(200, "application/json", "{\"ok\":true,\"started\":true,\"target\":\"" + jsonEscape(latestTag) + "\"}");
    server.client().stop();
    performUpdate();
  });
  // Two handlers: the second streams the file, the first replies once it is in.
  server.on("/update", HTTP_POST,
    [] {
      if (uploadRejected) { sendJsonError(403, "missing X-7seg header"); return; }
      // Not !Update.hasError(): that is false on a fresh Update object too, so a
      // POST with no file used to reboot the clock.
      if (!uploadOk) { server.send(400, "text/plain", "no valid firmware received"); return; }
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", "ok");
      delay(400);
      ESP.restart();
    },
    handleUpdateUpload);
  // The captive portal needs every unknown path to land on the form; on the
  // real network an unknown path is a 404, not the settings page with a 200.
  server.onNotFound([] {
    if (portalUp) handleRoot();
    else server.send(404, "text/plain", "not found");
  });
  server.begin();
}

// Without this the only way to find the clock is to read its IP off the USB
// serial line, and DHCP will move it whenever the router reboots.
//
// ⚠ MDNS.begin() twice without an end() between hangs the mDNS task. It is
// reachable from three places here (boot, leaving the portal, and the portal's
// own retry), so the guard is not optional — it froze the clock the first time
// it came home to a different network. ArduinoOTA.begin() used to call
// MDNS.begin() as well; that is disabled below and its service added here, so
// there is exactly one owner of the mDNS responder.
bool mdnsUp = false;
bool otaUp  = false;

void startMdns() {
  if (mdnsUp) MDNS.end();
  mdnsUp = false;
  if (MDNS.begin(hostName)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.enableArduino(3232, false);      // what ArduinoOTA.begin() would have advertised
    mdnsUp = true;
    Serial.printf("[MDNS] http://%s.local/\n", hostName);
  } else {
    Serial.println("[MDNS] failed");
  }
}

void startOta() {
  if (otaUp) return;
  otaUp = true;
  ArduinoOTA.setHostname(hostName);
  ArduinoOTA.setMdnsEnabled(false);       // startMdns() owns the responder
  ArduinoOTA.onStart([]() { confirmBeforeRestart(); showWord("UPd"); });   // a developer pushing is reachability too
  ArduinoOTA.onEnd([]()   { showWord("donE"); });
  ArduinoOTA.onError([](ota_error_t) { holdWord("Err"); });
  ArduinoOTA.begin();
  Serial.printf("[OTA] armed as %s\n", hostName);
}

// ---------------------------------------------------------------- wifi/ntp
// SNTP calls this when a reply actually lands. It is the ONLY thing that says the
// time is trustworthy: the old resync path took getLocalTime() succeeding as proof,
// which it always does once the clock has been set even once, so a network that
// later blocked NTP drifted for months with "[NTP] ok" in the log.
void onTimeSynced(struct timeval*) {
  timeValid = true;
  lastRealSyncMs = millis();
  Serial.println("[NTP] synced");
}

// Kick SNTP. Non-blocking: the callback above flips timeValid when the reply
// arrives, and loop() keeps serving the web page and OTA meanwhile. The old
// syncTime() spun for 15 s per call and was called every loop pass while NTP was
// down, so the device answered HTTP once every 15 s and looked hung.
void startNtp() {
  configTzTime(cfg.tz, NTP_SERVER);
  ntpStarted = true;
  lastNtpTryMs = millis();
}

void readCreds(String& ssid, String& pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
}

/** Begin an association with the saved network and return at once. */
bool beginSta() {
  String ssid, pass;
  readCreds(ssid, pass);
  if (ssid.isEmpty()) return false;
  Serial.printf("[WIFI] connecting to %s\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  return true;
}

/** Boot-time association: one attempt, blocking with a spinner, so the common
 *  case (router up) gets the time on the panel as fast as possible. */
bool tryConnect() {
  if (!beginSta()) return false;
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    paintFrame();
    delay(20);
  }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[WIFI] not yet"); return false; }
  Serial.printf("[WIFI] %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void locateIfNeeded() {
  // Locate once and cache. 0,0 is in the Atlantic, so it doubles as "unset".
  // Runs on ANY online transition, not only the boot path: a clock that came up
  // through the portal's retry used to never locate, so its temperature card
  // stayed dark until a reboot with the network already up.
  // A 1.1.0 clock already has lat/lon but no stored city; run once more for it.
  if ((cfg.lat != 0 || cfg.lon != 0) && cfg.city[0]) return;
  // At most once an hour: on a LAN with no internet this blocks for the HTTP
  // timeout, and it used to do so on every reconnect.
  static uint32_t lastTryMs = 0;
  if (lastTryMs && millis() - lastTryMs < 3600000UL) return;
  lastTryMs = millis();
  String city;
  if (geoLocate(cfg.lat, cfg.lon, city)) {
    setField(cfg.city, sizeof cfg.city, city);
    saveSettings(cfg);
  }
}

// Everything that should happen when the clock gains the network, from whichever
// state it came: boot, a drop that healed, or the portal's background retry.
void onOnline() {
  wasOnline = true;
  everOnline = true;
  lastConnectedMs = millis();
  Serial.println("[STATE] -> online");
  startWeb();
  startMdns();
  startOta();
  if (!timeValid) startNtp();
  locateIfNeeded();
}

void startPortal() {
  if (portalUp) return;
  portalUp = true;
  Serial.println("[STATE] -> portal");
  // Drop mDNS before changing interface mode. Leaving it registered across an
  // AP_STA switch is the other half of the same hang.
  if (mdnsUp) { MDNS.end(); mdnsUp = false; }
  // SILENCE THE STATION FIRST. The C3 has one radio. A station hunting for a
  // network that is not there (wrong SSID, router down) scans every channel
  // continuously, and while it does the soft AP barely beacons — found on the
  // bench: portal "up" for minutes and invisible to a phone. So the station's
  // auto-reconnect goes off and its attempt is cancelled; the portal retries it
  // on its own schedule, in short bounded bursts (see loop()).
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);     // stop the attempt; keep the radio on and the credentials
  WiFi.mode(WIFI_AP_STA);            // AP_STA, not AP: we keep retrying the
                                     // saved network while the portal is up.
  WiFi.softAP(apSsid, AP_PASSWORD);
  // softAP() returns before the interface has its address; binding DNS to
  // 0.0.0.0 gives a portal that serves pages but never triggers the sign-in
  // sheet, which is a miserable thing to debug.
  for (uint8_t i = 0; i < 20 && WiFi.softAPIP() == IPAddress(0, 0, 0, 0); i++) delay(50);
  // Without a DNS server answering every lookup with our own address, phones
  // never pop the sign-in sheet — captive detection fails at DNS, not at HTTP,
  // so onNotFound alone does nothing for it.
  dns.start(53, "*", WiFi.softAPIP());
  if (!webUp) { startWeb(); }
  lastPortalTryMs = millis();
  Serial.printf("[AP] %s  http://%s/\n", apSsid, WiFi.softAPIP().toString().c_str());
  if (!timeValid) showWord("AP");    // once the time is known, paintFrame() keeps showing it
}

void stopPortal() {
  if (!portalUp) return;
  portalUp = false;
  dns.stop();
  // The web server stays up — it is the settings page now, not the portal.
  portalTryUntilMs = 0;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);       // back to normal: the driver rides out short drops itself
}

// ---------------------------------------------------------------- lifecycle
void setup() {
  Serial.begin(115200);
  // USB CDC enumerates in 1-2 s; without this the one line telling you the
  // portal's address is emitted at ~100 ms and lost.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  loadSettings(cfg);                 // defaults until the page has been used
  snprintf(hostName, sizeof hostName, "%s", cfg.name);
  snprintf(apSsid, sizeof apSsid, "%s-clock", cfg.name);

  // FastLED takes the pin as a TEMPLATE parameter, not an argument — the
  // clockless driver's bit timing is generated at compile time. So a runtime
  // pin means instantiating the driver once per candidate and picking here,
  // and changing the setting needs a reboot. There is no way around it short
  // of hand-rolling the RMT setup.
  switch (cfg.dataPin) {
#define X(n) case n: ledController = &FastLED.addLeds<WS2812, n, GRB>(leds, activeLedCount(cfg)); break;
    PIN_XLIST
#undef X
    // Unreachable via /set, which validates against PIN_LIST — but NVS can hold
    // a pin from a build for a different chip, so it has to land somewhere.
    default: ledController = &FastLED.addLeds<WS2812, DEFAULT_DATA_PIN, GRB>(leds, activeLedCount(cfg));
             cfg.dataPin = DEFAULT_DATA_PIN; break;
  }
  bootPin = cfg.dataPin;             // the page shows "restart to use GPIO n" only while this differs
  FastLED.setBrightness(cfg.brightness);
  // NO POWER CAP, DELIBERATELY.
  //
  // There was one, at 500 mA, carrying a comment about 32 x 60 mA = 1.9 A. Both
  // numbers were inherited from somewhere else: 60 mA is the 5050 WS2812B
  // figure and this panel is built from WS2812B-2020 parts, and the 500 is
  // remembered as a DEV BOARD number, where the LEDs ran off 3.3 V -- a
  // REGULATOR budget, not a USB one. Two unrelated limits conflated into a
  // ceiling that then throttled white content to brightness 93/255.
  //
  // MEASURED 2026-09-21: every LED full white at brightness 255 with the
  // limiter lifted. No brownout, boot count unchanged, brownout counter zero.
  //
  // The safety net is now evidence rather than arithmetic: /api reports reset
  // reason and a persistent brownout count, and /stress reproduces the worst
  // case on demand. If the supply ever cannot hold it, the board says so.
  // ⚠ A build with 5050 parts on a small supply should reinstate a cap.

  // Count boots, and the subset that were brownouts. A brownout nobody watched
  // is otherwise indistinguishable from a power-cycle after the fact.
  {
    Preferences bp;
    if (bp.begin("boots", false)) {
      lastStressTry = bp.getUChar("try", 0);   // non-zero => died at this step
      bootCount = bp.getUInt("n", 0) + 1;
      brownoutCount = bp.getUInt("bo", 0);
      if (esp_reset_reason() == ESP_RST_BROWNOUT) brownoutCount++;
      bp.putUInt("n", bootCount);
      bp.putUInt("bo", brownoutCount);
      bp.end();
    }
  }
  Serial.printf("[PWR] boot #%lu, reset reason %s, brownouts so far %lu, fw " FW_VERSION "\n",
                (unsigned long)bootCount, resetReasonName(), (unsigned long)brownoutCount);
  // "pending" right after an OTA means the bootloader is tracking this image and
  // will roll back if it is not confirmed; "undefined" means it is not.
  Serial.printf("[OTA] running image state: %s\n", otaStateName());
  bootPending = imagePending();
  // Version first, before wifi: 1.5 s on a clock that then spends seconds joining
  // the network costs nothing, and after an update it confirms which image booted.
  if (showVersion()) { Serial.println("[BOOT] showing version " FW_VERSION); delay(1500); }
  showSpin(0);

  sntp_set_time_sync_notification_cb(onTimeSynced);
  setenv("TZ", cfg.tz, 1); tzset();  // so the first paint after a sync is already local
  WiFi.setHostname(hostName);        // the DHCP name the router shows; must precede mode()
  WiFi.mode(WIFI_STA);
  if (!hasCreds()) { startPortal(); return; }

  if (tryConnect()) {
    onOnline();
  } else {
    // Credentials exist, so this is an outage — a router still booting after
    // the same power cut, most likely — not a setup problem. Same ten-minute
    // grace as a drop in service; loop() reconnects and raises the portal only
    // after that. Raising it here put an open AP up after every power cut.
    startWeb();
    lastConnectedMs = millis();
  }
}

void loop() {
  uint32_t now = millis();
  confirmImage();                    // uptime-based; every branch reaches it

  if (portalUp) {
    dns.processNextRequest();
    pumpWeb();
    if (WiFi.status() == WL_CONNECTED) {
      // The portal is not a dead end. The background attempt landed: tear the
      // AP down and come up as a normal clock.
      stopPortal();
      onOnline();
    } else if (portalTryUntilMs && (int32_t)(now - portalTryUntilMs) > 0) {
      // The burst is over and it did not land: stop scanning so the AP is
      // visible again until the next burst.
      portalTryUntilMs = 0;
      WiFi.disconnect(false, false);
      Serial.println("[WIFI] saved network not found; portal visible again");
    } else if (!portalTryUntilMs && now - lastPortalTryMs > PORTAL_RETRY_MS) {
      // A bounded, non-blocking attempt: begin(), give it PORTAL_TRY_MS, then
      // go quiet (above). The AP is hard to see during those seconds, so while
      // a phone is attached the attempt waits — but not forever: a parked phone
      // must not pin the clock in the portal.
      static uint32_t lastForcedMs = 0;
      lastPortalTryMs = now;                       // stamped even with no creds, so hasCreds() is not polled every pass
      bool phoneAttached = WiFi.softAPgetStationNum() > 0;
      if (!phoneAttached || now - lastForcedMs > 5 * PORTAL_RETRY_MS) {
        lastForcedMs = now;
        if (hasCreds() && beginSta()) portalTryUntilMs = now + PORTAL_TRY_MS;
      }
    }
    paintFrame();                    // the time, if known, not "AP" forever
    delay(5);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    wasOnline = false;
    pumpWeb();
    if (now - lastReconnectMs > RECONNECT_EVERY_MS) {
      lastReconnectMs = now;
      Serial.println("[WIFI] reconnecting");
      WiFi.reconnect();
    }
    // Only after a genuinely long outage is it worth offering the portal — and
    // even then it keeps retrying in the background. timeValid is NOT cleared:
    // the clock keeps showing the last known time, dot lit as the tell. A boot
    // that has never associated gets the shorter grace: wrong credentials should
    // not cost ten minutes, and the portal keeps trying the saved network anyway.
    // A freshly installed image that cannot get online needs to become reachable
    // before the confirm window closes, or a power cut in that window rolls it
    // back for no reason of its own — so while the image is pending, the portal
    // comes up after the confirm interval instead.
    uint32_t grace = everOnline ? OFFLINE_TO_PORTAL_MS : (bootPending ? IMAGE_CONFIRM_MS : BOOT_OFFLINE_MS);
    if (now - lastConnectedMs > grace) startPortal();
    paintFrame();
    delay(5);
    return;
  }

  if (!wasOnline) onOnline();        // a drop healed, or the boot-time attempt landed late
  lastConnectedMs = now;
  pumpWeb();
  ArduinoOTA.handle();

  if (!timeValid) {
    // Waiting for the first reply. Re-kick SNTP once a minute in case the first
    // request went out before the resolver was ready; keep serving everything.
    if (!ntpStarted || now - lastNtpTryMs > NTP_RETRY_MS) startNtp();
    paintFrame();
    delay(5);
    return;
  }

  // The background SNTP poller refreshes every 3 h (CONFIG_LWIP_SNTP_UPDATE_DELAY
  // in the prebuilt core) and the C3's RTC free-runs on an uncalibrated
  // oscillator, so re-kick it every 6 h as well. Whether a reply arrives is
  // recorded by the callback, and /api reports minutes since the last real sync.
  if (now - lastResyncMs > RESYNC_AFTER_MS) { lastResyncMs = now; startNtp(); }

  // Automatic update check: a minute after the clock first has the time, then
  // once a day. A few seconds of spread from the MAC keep a houseful of clocks
  // that powered up together from asking GitHub in the same second. The check is
  // one small TLS request; the flashing dot and INSTALL mode both read updAvail.
  if (cfg.upd != UPD_OFF && !wiringOwnsPanel()) {
    if (!nextAutoCheckMs) {
      nextAutoCheckMs = now + AUTOCHECK_FIRST_MS + (uint32_t)(ESP.getEfuseMac() % 30000ULL);
    } else if ((int32_t)(now - nextAutoCheckMs) >= 0) {
      bool ok = checkUpdate();
      nextAutoCheckMs = now + (ok ? AUTOCHECK_EVERY_MS : AUTOCHECK_RETRY_MS);
      Serial.printf("[UPD] auto-check: %s\n",
                    !ok ? "failed, retry in 1 h" : updAvail ? ("update available: " + latestTag).c_str() : "up to date");
    }
  }
  // INSTALL mode: once, in the small hours, if the last check found something.
  // Re-checked immediately before installing, so a release pulled in the
  // meantime is never installed.
  if (cfg.upd == UPD_INSTALL && updAvail && !wiringOwnsPanel()) {
    struct tm lt;
    if (getLocalTime(&lt, 0) && lt.tm_hour == AUTOINSTALL_HOUR && lt.tm_yday != lastInstallYday) {
      lastInstallYday = lt.tm_yday;
      if (checkUpdate() && updAvail) {
        Serial.printf("[UPD] installing %s overnight\n", latestTag.c_str());
        performUpdate();             // reboots on success
      }
    }
  }

  // Ticker: fetch on its own cache timer, scroll on the interval you set.
  // Guarded on timeValid so a clock that does not yet know the time never
  // wanders off to fetch a price.
  // The ticker paints straight into the buffer on its own path, so the gate in
  // showTime() does not cover it. A BTC scroll arriving mid-wizard would be a
  // baffling thing to be asked to identify.
  if (cfg.tickMins && !wiringOwnsPanel() &&
      now - lastTickMs > cfg.tickMins * 60000UL) {
    lastTickMs = now;
    struct tm t;
    if (getLocalTime(&t, 50)) {
      if ((cfg.cards & CARD_BTC) && tickerFetch(btc))
        tickerScroll(leds, btc, cfg, t, pumpWeb);
      if ((cfg.cards & CARD_TEMP) && (cfg.lat || cfg.lon) && tickerWeather(btc, cfg.lat, cfg.lon))
        tempShow(leds, btc, cfg, t, pumpWeb);
    }
  }

  paintFrame();
  delay(5);
}
