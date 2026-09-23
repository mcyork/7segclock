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
 * REPOS. github.com/mcyork/7segclock is the canonical home of this firmware and
 * the only place releases are cut. github.com/mcyork/mini7seg holds the segment
 * library it depends on, the enclosure, and a synced copy of this source at
 * firmware/ntp4digit/ for building against the library checkout. The two copies
 * are kept byte-identical; edit one, copy to the other.
 *
 * WIRING  (three wires, all on one edge of the C3 Super Mini)
 *   5V   -> panel H1 V
 *   GND  -> panel H1 G
 *   GPIO -> panel H1 DI        see DATA_PIN below
 *
 *   The four digit boards chain H2 (DO) of one into H1 (DI) of the next, so the
 *   panel is one 32-LED strip: digit 0 is LEDs 0-7, digit 3 is LEDs 24-31, each
 *   in segment order A,B,C,D,E,F,G,DP.
 *
 * ⚠ 3.3 V DATA INTO 5 V LEDS IS OUT OF SPEC.
 *   WS2812 wants V_IH = 0.7 x VDD = 3.5 V; a C3 pin gives 3.3 V. It usually
 *   works, which is what makes it nasty — the failure is intermittent, shows up
 *   as the first digit flickering or the chain freezing, and tracks temperature
 *   and lead length. If it misbehaves, cheapest fixes in order: a series
 *   Schottky in the LED's 5 V feed (drops VDD to ~4.3 V, so V_IH ~3.0 V), a
 *   74AHCT125 buffer, or burn LED 0 as a sacrificial level-shifting pixel.
 *
 * ⚠ DATA_PIN AVOIDS THE STRAPPING PINS. On the C3, GPIO2, GPIO8 and GPIO9 are
 *   sampled at reset to choose the boot mode. A WS2812 line idles low, and a
 *   strapping pin held the wrong way at power-up puts the chip into download
 *   mode instead of running your sketch — which looks exactly like a dead
 *   board. GPIO4 is not strapped and is safe.
 *
 * THE PORTAL IS NEVER A DEAD END
 *   Earlier revision got this badly wrong: any NTP failure at boot, or ~40 s of
 *   router downtime, parked the clock in the setup portal FOREVER, because
 *   nothing in loop() could ever leave portal mode. A router firmware update
 *   was enough to require physical intervention.
 *   Now: losing wifi never shows the portal quickly, the portal keeps retrying
 *   saved credentials in the background, and NTP failure is not a wifi problem
 *   so it never triggers the portal at all.
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
#include "String7Segment.h"
#include "settings.h"
#include "webui.h"
#include "geometryui.h"
#include <esp_system.h>

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

// ---------------------------------------------------------------- hardware
#define DATA_PIN     4            // not 2/8/9 — see strapping note above
// Brightness now lives in Settings so the web page can change it; this is only
// the value a virgin device starts at.
#define BRIGHTNESS   40           // WS2812 at close range; 40 is already bright

// ---------------------------------------------------------------- behaviour
#define FW_VERSION   "1.1.0"
// Self-update straight from GitHub Releases. The /latest/download/ path always
// resolves to the newest release's asset, so the device needs no version file
// to be maintained alongside the binary — the release IS the manifest.
#define GH_REPO      "mcyork/7segclock"
#define GH_LATEST    "https://api.github.com/repos/" GH_REPO "/releases/latest"
#define GH_BIN       "https://github.com/" GH_REPO "/releases/latest/download/firmware.bin"

#define HOSTNAME     "mini7seg"   // -> http://mini7seg.local/
#define AP_SSID      "mini7seg-clock"
#define AP_PASSWORD  "sevenseg"   // >= 8 chars or the AP silently refuses to start
#define NTP_SERVER   "pool.ntp.org"

// POSIX TZ, not a fixed GMT offset. An offset needs hand-editing twice a year;
// this string carries the DST rules with it and localtime_r applies them live,
// so the clock rolls at the right instant with no resync.
#define TZ_STRING    "PST8PDT,M3.2.0/2,M11.1.0/2"   // America/Los_Angeles

#define WIFI_TIMEOUT_MS      15000UL   // one association attempt
#define NTP_TIMEOUT_MS       15000UL
#define RECONNECT_EVERY_MS   20000UL   // while online-but-dropped
#define OFFLINE_TO_PORTAL_MS 600000UL  // 10 min down before offering the portal
#define PORTAL_RETRY_MS      60000UL   // portal keeps trying saved creds
#define RESYNC_AFTER_MS      21600000UL // force an NTP resync every 6 h

#define SSID_MAX  32              // 802.11 limits; longer silently never associates
#define PASS_MAX  63

// ---------------------------------------------------------------- globals
CRGB leds[MAX_LEDS];
Preferences prefs;
WebServer server(80);
DNSServer dns;
CLEDController* ledController = nullptr;

bool portalUp   = false;
bool timeValid  = false;
uint32_t lastConnectedMs = 0;
uint32_t lastReconnectMs = 0;
uint32_t lastPortalTryMs = 0;
uint32_t lastSyncMs      = 0;
Settings cfg;

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
String geoCity;
uint32_t lastTickMs = 0;
bool webUp = false;

enum PreviewMode : uint8_t { PREVIEW_OFF = 0, PREVIEW_IDENTIFY, PREVIEW_PROBE };
PreviewMode previewMode = PREVIEW_OFF;
uint8_t previewDigit = 0, previewSegment = 0, previewGroup = 0;
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
    uint16_t base = (uint16_t)previewGroup * cfg.ledsPerSeg;
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

bool pumpWeb() {
  server.handleClient();
  return serviceWiringPreview();
}

// ⚠ Only these render: 0-9 A-F H J L N O P R U Y - _ and space. Anything else
// comes back as a blank digit, silently. "Sync" and "boot" were the first draft
// of this and both had a letter the library cannot draw (S and t), so they
// showed as holes. Any digit that would blank lights its DP as a tell.
void showWord(const char* s) {
  clearDisplay(leds, cfg);
  for (uint8_t i = 0; i < cfg.digits && s[i]; i++) {
    renderChar(leds, cfg, i, s[i], CRGB(cfg.r, cfg.g, cfg.b));
    if (String7Segment::getPattern(s[i]) == 0 && s[i] != ' ')
      renderDecimalPoint(leds, cfg, i, CRGB(cfg.r, cfg.g, cfg.b));   // "this character does not exist"
  }
  FastLED.show();
}

// Busy indicator for blocking waits. A static word during a 15 s connect looks
// like a hung board; a moving one does not. No letters, so no character-set risk.
void showSpin(uint8_t step) {
  static const uint8_t SPIN_SEGMENTS[] = { SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F };
  clearDisplay(leds, cfg);
  for (uint8_t d = 0; d < cfg.digits; d++)
    renderDigit(d, SPIN_SEGMENTS[(step + d) % 6], CRGB(cfg.r, cfg.g, cfg.b));
  FastLED.show();
}

void showTime() {
  // The single gate. showTime() is reached from three places -- the normal loop,
  // the wifi-dropped branch, and the post-ticker repaint -- and guarding each of
  // them was how the preview got painted over in the first place. One refusal
  // here covers every caller, including any added later.
  if (wiringOwnsPanel()) return;

  struct tm t;
  if (!getLocalTime(&t, 50)) return;

  int hh = t.tm_hour, mm = t.tm_min;
  if (cfg.hour12) { hh = hh % 12; if (hh == 0) hh = 12; }

  uint8_t d[4] = { (uint8_t)(hh / 10), (uint8_t)(hh % 10),
                   (uint8_t)(mm / 10), (uint8_t)(mm % 10) };
  uint32_t ms = millis();

  clearDisplay(leds, cfg);
  // Foreground is set per digit rather than once: that is what lets SPECTRUM
  // spread across the display, and the modes that ignore position simply
  // return the same colour four times.
  uint8_t shown = min<uint8_t>(4, cfg.digits);
  for (uint8_t i = 0; i < shown; i++) {
    if (i == 0 && cfg.hour12 && hh < 10) continue;   // blank, not zero-padded
    CRGB c = colourFor(cfg, i, t, ms);
    renderDigit(i, String7Segment::getPattern((char)('0' + d[i])), c);
  }

  // No colon on this panel, so digit 1's decimal point stands in — blinking on
  // the second by default, which also makes "still alive" visible for free.
  bool dp = cfg.colon == COLON_ON  ? true
          : cfg.colon == COLON_OFF ? false
          : (t.tm_sec % 2 == 0);
  if (dp) {
    CRGB c = colourFor(cfg, 1, t, ms);
    renderDecimalPoint(leds, cfg, 1, c);
  }

  // Seconds overlay goes on LAST, straight into the pixel buffer, because it
  // has to know which segments the digits actually lit — that masking is the
  // whole idea. Doing it through the display API would mean re-deriving the
  // patterns the library just finished drawing.
  applySeconds(leds, activeLedCount(cfg), secondsNow(t), cfg, colourFor(cfg, 0, t, ms));

  // Offline shows a dot on digit 3 as well: the time is the last known good
  // one and may be drifting. Blanking the display instead would be worse — a
  // clock that goes dark every time the router hiccups is useless.
  if (WiFi.status() != WL_CONNECTED && cfg.digits > 3)
    renderDecimalPoint(leds, cfg, 3, colourFor(cfg, 3, t, ms));
  FastLED.show();
}

// ---------------------------------------------------------------- credentials
bool hasCreds() {
  prefs.begin("wifi", true);
  bool have = prefs.getString("ssid", "").length() > 0;
  prefs.end();
  return have;
}

// ---------------------------------------------------------------- portal
const char PORTAL_HTML[] PROGMEM =
  "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>mini7seg clock</title>"
  "<style>body{font:16px system-ui;margin:2rem auto;max-width:22rem;padding:0 1rem}"
  "input,button{font:inherit;width:100%;padding:.6rem;margin:.3rem 0;box-sizing:border-box}"
  "button{background:#111;color:#fff;border:0;border-radius:.3rem}</style>"
  "<h2>mini7seg clock</h2><form method=post action=/save>"
  "<input name=ssid placeholder='WiFi network' maxlength=32 required>"
  "<input name=pass type=password placeholder='Password' maxlength=63>"
  "<button>Save and restart</button></form>";

void handleRoot() {
  // One route, two pages: the credentials form while the portal is up, the
  // settings page once we are on a real network.
  server.send_P(200, "text/html; charset=utf-8", portalUp ? PORTAL_HTML : SETTINGS_HTML);
}

void sendState() {
  struct tm t;
  char clockStr[16] = "--:--";
  if (timeValid && getLocalTime(&t, 50))
    snprintf(clockStr, sizeof clockStr, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);

  char buf[780];
  snprintf(buf, sizeof buf,
    "{\"hue\":%u,\"spread\":%u,\"env\":%u,\"secpath\":%u,\"sectrail\":%u,"
    "\"r\":%u,\"g\":%u,\"b\":%u,\"bri\":%u,"
    "\"rst\":\"%s\",\"boots\":%u,\"brownouts\":%u,\"upMin\":%lu,"
    "\"estMw\":%lu,\"diedAt\":%u,"
    "\"h12\":%s,\"colon\":%u,\"speed\":%u,\"tick\":%u,\"cards\":%u,"
    "\"digits\":%u,\"ledsPerSeg\":%u,\"dpMask\":%u,"
    "\"fw\":\"%s\",\"pin\":%u,\"pins\":[%s],\"btc\":%ld,\"tempF\":%.1f,\"city\":\"%s\","
    "\"time\":\"%s\",\"ip\":\"%s\"}",
    cfg.hue, cfg.spread, cfg.env, cfg.secpath, cfg.sectrail,
    cfg.r, cfg.g, cfg.b, cfg.brightness,
    resetReasonName(), bootCount, brownoutCount, (unsigned long)(millis() / 60000UL),
    // What FastLED BELIEVES the panel draws with the current buffer at full
    // brightness, and the brightness its cap would allow. briCap below
    // cfg.brightness means the display is being throttled -- and FastLED models
    // 5050-class parts, so on a 2020 panel that throttle is largely spurious.
    (unsigned long)calculate_unscaled_power_mW(leds, activeLedCount(cfg)),
    lastStressTry,
    cfg.hour12 ? "true" : "false", cfg.colon, cfg.speed, cfg.tickMins, cfg.cards,
    cfg.digits, cfg.ledsPerSeg, cfg.dpMask,
    FW_VERSION, cfg.dataPin, pinListJson().c_str(), btc.usd, btc.wxOk ? btc.tempF : 0.0f, geoCity.c_str(), clockStr,
    WiFi.localIP().toString().c_str());
  server.send(200, "application/json", buf);
}

void handleSet() {
  static const char* const allowed[] = {
    "hue", "spread", "env", "secpath", "sectrail", "r", "g", "b", "bri",
    "colon", "speed", "tick", "cards", "pin", "h12"
  };
  if (!rejectUnexpectedArgs(allowed, sizeof(allowed) / sizeof(allowed[0]))) return;

  if (!readOptionalU8("hue",    cfg.hue,        0, HUE_COUNT - 1)) return;
  if (!readOptionalU8("spread", cfg.spread,     0, 64)) return;
  if (!readOptionalU8("env",    cfg.env,        0, ENV_COUNT - 1)) return;
  if (!readOptionalU8("secpath",  cfg.secpath,  0, PATH_COUNT - 1)) return;
  if (!readOptionalU8("sectrail", cfg.sectrail, 0, TRAIL_COUNT - 1)) return;
  if (!readOptionalU8("r",      cfg.r,          0, 255)) return;
  if (!readOptionalU8("g",      cfg.g,          0, 255)) return;
  if (!readOptionalU8("b",      cfg.b,          0, 255)) return;
  if (!readOptionalU8("bri",    cfg.brightness, 5, 255)) return;
  if (!readOptionalU8("colon",  cfg.colon,      0, COLON_OFF)) return;
  if (!readOptionalU8("speed",  cfg.speed,      1, 20)) return;
  if (!readOptionalU8("tick",   cfg.tickMins,   0, 60)) return;
  if (!readOptionalU8("cards",  cfg.cards,      0, 3)) return;
  // Validated against the allow-list, not just a range: an arbitrary pin would
  // fall through to the default and silently keep using 4, which looks like the
  // setting simply does not work.
  if (server.hasArg("pin")) {
    long want;
    if (!parseLongStrict(server.arg("pin"), want)) {
      sendJsonError(400, "pin must be an integer");
      return;
    }
    bool ok = false;
    for (uint8_t p : PIN_LIST) if (p == want) { ok = true; break; }
    if (!ok) { sendJsonError(400, "pin is not in this chip's allow-list"); return; }
    cfg.dataPin = (uint8_t)want;
  }
  if (server.hasArg("h12")) {
    long h12;
    if (!parseLongStrict(server.arg("h12"), h12)) { sendJsonError(400, "h12 must be an integer"); return; }
    if (h12 < 0 || h12 > 1) { sendJsonError(400, "h12 must be 0..1"); return; }
    cfg.hour12 = h12 != 0;
  }

  FastLED.setBrightness(cfg.brightness);
  saveSettings(cfg);        // survives a power cut; the clock is a fixture
  sendState();
}

String geometryJson() {
  String out;
  out.reserve(360);
  out += "{\"digits\":";
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
  static const char* const allowed[] = { "i" };
  if (!rejectUnexpectedArgs(allowed, sizeof(allowed) / sizeof(allowed[0]))) return;
  long i;
  if (!readLongArg("i", -1, (activeLedCount(cfg) / cfg.ledsPerSeg) - 1, i)) return;
  if (i < 0) {
    cancelPreview();
    clearDisplay(leds, cfg);
    FastLED.show();
    server.send(200, "application/json", "{\"ok\":true,\"mode\":\"off\"}");
    return;
  }

  previewMode = PREVIEW_PROBE;
  previewDirty = true;
  previewGroup = (uint8_t)i;
  previewTouchedMs = millis();
  serviceWiringPreview();
  server.send(200, "application/json", "{\"ok\":true,\"mode\":\"probe\"}");
}

void handleSave() {
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
  if (ok) ok = (pass.isEmpty() || prefs.putString("pass", pass) > 0);
  prefs.end();

  if (!ok) { sendJsonError(500, "could not write wifi settings to flash"); return; }

  server.send(200, "text/html; charset=utf-8", "<meta name=viewport content='width=device-width'>"
              "<p style=\"font:16px system-ui\">Saved. Restarting&hellip;</p>");
  delay(600);
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
 x.open('POST','/update');x.send(d);};
</script>)HTML";

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[UPD] %s\n", up.filename.c_str());
    showWord("oTA");
    // UPDATE_SIZE_UNKNOWN: the browser does not tell us the length up front, so
    // let the Update library size it against the free OTA partition.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) { Serial.printf("[UPD] ok, %u bytes\n", up.totalSize); showWord("donE"); }
    else { Update.printError(Serial); showWord("Err"); }
  }
}

String latestTag;          // "" until a check has run

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
 *  Check for updates — there is no automatic check. */
bool checkUpdate() {
  String b = httpGet(GH_LATEST, true);
  int k = b.indexOf("\"tag_name\":\"");
  if (k < 0) return false;
  int e = b.indexOf('"', k + 12);
  latestTag = b.substring(k + 12, e);
  if (latestTag.startsWith("v")) latestTag = latestTag.substring(1);
  Serial.printf("[UPD] running %s, latest %s\n", FW_VERSION, latestTag.c_str());
  return true;
}

void startWeb() {
  if (webUp) return;
  webUp = true;
  server.on("/",     handleRoot);
  server.on("/api",  sendState);
  server.on("/set",  handleSet);
  server.on("/geometry", HTTP_GET, handleGeometry);
  server.on("/geometry", HTTP_POST, handleGeometry);
  server.on("/setgeometry", handleSetGeometry);
  server.on("/identify", handleIdentify);
  server.on("/probe", handleProbe);
  server.on("/save", HTTP_POST, handleSave);
  // Deliberately POST: a GET /reboot would be followed by any link prefetcher
  // or crawler that ever saw the page, and rebooting the clock by accident is
  // a rotten way to find that out.
  server.on("/reboot", HTTP_POST, [] {
    if (!rejectNoArgs()) return;
    server.send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
  });
  // Every LED, full white, for a few seconds: the worst case the panel can ever
  // present. Blocking on purpose -- loop() is not running inside a handler, so
  // the clock cannot repaint over it and no preview state machine is needed.
  //
  // Reports what FastLED BELIEVES it costs and what its cap allowed. If the
  // board survives this, no display content can brown it out; if it resets, the
  // brownout counter in /api will say so on the next boot, which is exactly the
  // evidence a USB power meter would have given.
  server.on("/stress", [] {
    long ms = 3000;
    if (server.hasArg("ms") && !parseLongStrict(server.arg("ms"), ms)) ms = 3000;
    if (ms < 100) ms = 100;
    if (ms > 10000) ms = 10000;
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
      uint32_t t1 = millis();
      while (millis() - t1 < (uint32_t)ms / 4) { server.handleClient(); delay(5); }
      reached = step;
    }
    if (sp.begin("boots", false)) { sp.putUChar("try", 0); sp.end(); }   // survived

    FastLED.setBrightness(savedBri);
    clearDisplay(leds, cfg);
    FastLED.show();

    char b[260];
    snprintf(b, sizeof b,
      "{\"ok\":true,\"leds\":%u,\"heldMs\":%ld,"
      "\"modelMw\":%lu,\"modelMa\":%lu,"
      "\"reachedBrightness\":%u,\"survived\":true}",
      n, ms, (unsigned long)mw, (unsigned long)(mw / 5), reached);
    server.send(200, "application/json", b);
  });
  server.on("/wiring", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", GEOMETRY_HTML); });
  server.on("/update", HTTP_GET, [] {
    if (!rejectNoArgs()) return;
    server.send_P(200, "text/html; charset=utf-8", UPDATE_HTML);
  });
  server.on("/checkupdate", [] {
    if (!rejectNoArgs()) return;
    bool ok = checkUpdate();
    char b[160];
    snprintf(b, sizeof b, "{\"ok\":%s,\"current\":\"%s\",\"latest\":\"%s\",\"newer\":%s}",
             ok ? "true" : "false", FW_VERSION, latestTag.c_str(),
             (ok && isNewer(latestTag, FW_VERSION)) ? "true" : "false");
    server.send(200, "application/json", b);
  });
  server.on("/doupdate", [] {
    if (!rejectNoArgs()) return;
    // Reply BEFORE flashing: the download takes ~20 s and the connection would
    // otherwise time out, leaving the page unable to say whether it worked.
    server.send(200, "application/json", "{\"started\":true}");
    server.client().stop();
    showWord("oTA");
    WiFiClientSecure tls; tls.setInsecure();
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GH redirects to its CDN
    httpUpdate.rebootOnUpdate(true);
    t_httpUpdate_return r = httpUpdate.update(tls, GH_BIN);
    if (r == HTTP_UPDATE_FAILED) {
      Serial.printf("[UPD] failed %d: %s\n", httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      showWord("Err");
    }
  });
  // Two handlers: the second streams the file, the first replies once it is in.
  server.on("/update", HTTP_POST,
    [] {
      bool ok = !Update.hasError();
      server.sendHeader("Connection", "close");
      server.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "update failed");
      if (ok) { delay(400); ESP.restart(); }
    },
    handleUpdateUpload);
  server.onNotFound(handleRoot);     // also what makes the captive portal work
  server.begin();
}

// Without this the only way to find the clock is to read its IP off the USB
// serial line, and DHCP will move it whenever the router reboots.
//
// ⚠ MDNS.begin() twice without an end() between hangs the mDNS task. It is
// reachable from three places here (boot, leaving the portal, and the portal's
// own retry), so the guard is not optional — it froze the clock the first time
// it came home to a different network.
bool mdnsUp = false;
void startOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]() { showWord("oTA"); });
  ArduinoOTA.onEnd([]()   { showWord("donE"); });
  ArduinoOTA.onError([](ota_error_t) { showWord("Err"); });
  ArduinoOTA.begin();
  Serial.printf("[OTA] armed as %s\n", HOSTNAME);
}

void startMdns() {
  if (mdnsUp) MDNS.end();
  mdnsUp = false;
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsUp = true;
    Serial.printf("[MDNS] http://%s.local/\n", HOSTNAME);
  } else {
    Serial.println("[MDNS] failed");
  }
}

void startPortal() {
  if (portalUp) return;
  portalUp = true;
  Serial.println("[STATE] -> portal");
  // Drop mDNS before changing interface mode. Leaving it registered across an
  // AP_STA switch is the other half of the same hang.
  if (mdnsUp) { MDNS.end(); mdnsUp = false; }
  WiFi.mode(WIFI_AP_STA);            // AP_STA, not AP: we keep retrying the
                                     // saved network while the portal is up.
  WiFi.softAP(AP_SSID, AP_PASSWORD);
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
  Serial.printf("[AP] %s  http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  showWord("AP");
}

void stopPortal() {
  if (!portalUp) return;
  portalUp = false;
  dns.stop();
  // The web server stays up — it is the settings page now, not the portal.
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  Serial.println("[STATE] -> online");
  startMdns();
  startOta();
}

// ---------------------------------------------------------------- wifi/ntp
bool tryConnect() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.isEmpty()) return false;

  Serial.printf("[WIFI] connecting to %s\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t t0 = millis();
  uint8_t step = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    if (!timeValid) showSpin(step++);   // once the time is known, keep showing it
    else showTime();
    delay(120);
  }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[WIFI] failed"); return false; }
  Serial.printf("[WIFI] %s\n", WiFi.localIP().toString().c_str());
  lastConnectedMs = millis();
  return true;
}

bool syncTime() {
  configTzTime(TZ_STRING, NTP_SERVER);
  struct tm t;
  uint32_t t0 = millis();
  uint8_t step = 0;
  while (millis() - t0 < NTP_TIMEOUT_MS) {
    // getLocalTime() already rejects the 1970 default (it requires tm_year >
    // 2016-1900), so this is belt-and-braces rather than the load-bearing check
    // the old comment claimed.
    if (getLocalTime(&t, 200) && t.tm_year > 120) {
      timeValid = true; lastSyncMs = millis();
      Serial.println("[NTP] ok");
      return true;
    }
    if (!timeValid) showSpin(step++); else showTime();
    delay(120);
  }
  Serial.println("[NTP] failed");
  return false;
}

// ---------------------------------------------------------------- lifecycle
void setup() {
  Serial.begin(115200);
  // USB CDC enumerates in 1-2 s; without this the one line telling you the
  // portal's address is emitted at ~100 ms and lost.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  loadSettings(cfg);                 // defaults until the page has been used

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
  Serial.printf("[PWR] boot #%u, reset reason %s, brownouts so far %u\n",
                bootCount, resetReasonName(), brownoutCount);
  showSpin(0);

  WiFi.mode(WIFI_STA);
  if (!hasCreds()) { startPortal(); return; }

  if (tryConnect()) {
    startWeb();
    startMdns();
    startOta();
    // Locate once and cache. 0,0 is in the Atlantic, so it doubles as "unset".
    if (cfg.lat == 0 && cfg.lon == 0 && geoLocate(cfg.lat, cfg.lon, geoCity)) saveSettings(cfg);
    // NTP failing is NOT a wifi problem. On a cold start after a power cut the
    // router and the clock boot together and the clock wins the race to the
    // internet; treating that as "bad credentials" used to send a perfectly
    // configured device to the setup portal.
    syncTime();
  } else {
    startPortal();
  }
}

void loop() {
  if (portalUp) {
    dns.processNextRequest();
    server.handleClient();

    // The portal is not a dead end. If credentials exist, keep trying them —
    // the network may simply have been down when we gave up.
    if (millis() - lastPortalTryMs > PORTAL_RETRY_MS && hasCreds()) {
      lastPortalTryMs = millis();
      if (tryConnect()) {
        // stopPortal() tears the AP down and brings mDNS up. Calling
        // startMdns() here as well was a double MDNS.begin() with no end()
        // between — and the first ran while the softAP was still up.
        stopPortal();
        if (!timeValid) syncTime();
      }
    }
    delay(5);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Keep showing the last known time, with digit 3's DP lit to say so.
    showTime();
    server.handleClient();
    if (millis() - lastReconnectMs > RECONNECT_EVERY_MS) {
      lastReconnectMs = millis();
      Serial.println("[WIFI] dropped, reconnecting");
      WiFi.reconnect();
    }
    // Only after a genuinely long outage is it worth offering the portal — and
    // even then it keeps retrying in the background.
    if (millis() - lastConnectedMs > OFFLINE_TO_PORTAL_MS) {
      timeValid = false;               // force a resync once we are back
      startPortal();
    }
    delay(200);
    return;
  }

  lastConnectedMs = millis();
  server.handleClient();             // settings page, while the clock runs
  ArduinoOTA.handle();
  if (!timeValid) { syncTime(); return; }

  // The background SNTP poller refreshes every 3 h (CONFIG_LWIP_SNTP_UPDATE_DELAY
  // in the prebuilt core) and the C3's RTC free-runs on an uncalibrated
  // oscillator, so pin it down periodically.
  if (millis() - lastSyncMs > RESYNC_AFTER_MS) syncTime();

  // Ticker: fetch on its own cache timer, scroll on the interval you set.
  // Guarded on timeValid so a clock that does not yet know the time never
  // wanders off to fetch a price.
  // The ticker paints straight into the buffer on its own path, so the gate in
  // showTime() does not cover it. A BTC scroll arriving mid-wizard would be a
  // baffling thing to be asked to identify.
  if (cfg.tickMins && timeValid && !wiringOwnsPanel() &&
      millis() - lastTickMs > cfg.tickMins * 60000UL) {
    lastTickMs = millis();
    struct tm t;
    if (getLocalTime(&t, 50)) {
      auto pump = [] { server.handleClient(); return serviceWiringPreview(); };
      if ((cfg.cards & CARD_BTC) && tickerFetch(btc))
        tickerScroll(leds, btc, cfg, t, pump);
      if ((cfg.cards & CARD_TEMP) && (cfg.lat || cfg.lon) && tickerWeather(btc, cfg.lat, cfg.lon))
        tempShow(leds, btc, cfg, t, pump);
    }
  }

  showTime();
  delay(200);
}
