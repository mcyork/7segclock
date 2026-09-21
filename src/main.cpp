/*
 * ntp4digit — a four-digit NTP clock on an ESP32-C3 Super Mini.
 *
 * Deliberately much smaller than the ntp_clock project: no buttons, no buzzer,
 * no OTA, no web installer. Join wifi, get the time, show it.
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
 * BUILD  (the library lives two levels up; nothing puts it on the path for you)
 *   arduino-cli lib install FastLED
 *   arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --library ../../src .
 *   arduino-cli upload  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc -p /dev/cu.usbmodem* .
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
#include "ticker.h"

// ---------------------------------------------------------------- hardware
#define DATA_PIN     4            // not 2/8/9 — see strapping note above
#define DIGITS       4
#define NUM_LEDS     (DIGITS * 8)
// Brightness now lives in Settings so the web page can change it; this is only
// the value a virgin device starts at.
#define BRIGHTNESS   40           // WS2812 at close range; 40 is already bright
#define MAX_MA       500          // hard ceiling; see note at setMaxPower below

// ---------------------------------------------------------------- behaviour
#define FW_VERSION   "1.0.0"
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
CRGB leds[NUM_LEDS];
String7Segment display(leds, 0, DIGITS);
Preferences prefs;
WebServer server(80);
DNSServer dns;

bool portalUp   = false;
bool timeValid  = false;
uint32_t lastConnectedMs = 0;
uint32_t lastReconnectMs = 0;
uint32_t lastPortalTryMs = 0;
uint32_t lastSyncMs      = 0;
Settings cfg;
Ticker btc;
String geoCity;
uint32_t lastTickMs = 0;
bool webUp = false;

// ---------------------------------------------------------------- display
// ⚠ Only these render: 0-9 A-F H J L N O P R U Y - _ and space. Anything else
// comes back as a blank digit, silently. "Sync" and "boot" were the first draft
// of this and both had a letter the library cannot draw (S and t), so they
// showed as holes. Any digit that would blank lights its DP as a tell.
void showWord(const char* s) {
  display.clear();
  for (uint8_t i = 0; i < DIGITS && s[i]; i++) {
    display.showChar(s[i], i);
    if (String7Segment::getPattern(s[i]) == 0 && s[i] != ' ')
      display.setDecimalPoint(i, true);   // "this character does not exist"
  }
  FastLED.show();
}

// Busy indicator for blocking waits. A static word during a 15 s connect looks
// like a hung board; a moving one does not. No letters, so no character-set risk.
void showSpin(uint8_t step) {
  display.clear();
  for (uint8_t d = 0; d < DIGITS; d++) display.spinStep((step + d) % 6, d);
  FastLED.show();
}

void showTime() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return;

  int hh = t.tm_hour, mm = t.tm_min;
  if (cfg.hour12) { hh = hh % 12; if (hh == 0) hh = 12; }

  uint8_t d[4] = { (uint8_t)(hh / 10), (uint8_t)(hh % 10),
                   (uint8_t)(mm / 10), (uint8_t)(mm % 10) };
  uint32_t ms = millis();

  display.clear();
  // Foreground is set per digit rather than once: that is what lets SPECTRUM
  // spread across the display, and the modes that ignore position simply
  // return the same colour four times.
  for (uint8_t i = 0; i < 4; i++) {
    if (i == 0 && cfg.hour12 && hh < 10) continue;   // blank, not zero-padded
    CRGB c = colourFor(cfg, i, t, ms);
    display.setForeground(S7Color(c.r, c.g, c.b));
    display.showDigit(d[i], i);
  }

  // No colon on this panel, so digit 1's decimal point stands in — blinking on
  // the second by default, which also makes "still alive" visible for free.
  bool dp = cfg.colon == COLON_ON  ? true
          : cfg.colon == COLON_OFF ? false
          : (t.tm_sec % 2 == 0);
  if (dp) {
    CRGB c = colourFor(cfg, 1, t, ms);
    display.setForeground(S7Color(c.r, c.g, c.b));
  }
  display.setDecimalPoint(1, dp);

  // Seconds overlay goes on LAST, straight into the pixel buffer, because it
  // has to know which segments the digits actually lit — that masking is the
  // whole idea. Doing it through the display API would mean re-deriving the
  // patterns the library just finished drawing.
  applySeconds(leds, NUM_LEDS, secondsNow(t), cfg, colourFor(cfg, 0, t, ms));

  // Offline shows a dot on digit 3 as well: the time is the last known good
  // one and may be drifting. Blanking the display instead would be worse — a
  // clock that goes dark every time the router hiccups is useless.
  if (WiFi.status() != WL_CONNECTED) display.setDecimalPoint(3, true);
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
  "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
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
  server.send_P(200, "text/html", portalUp ? PORTAL_HTML : SETTINGS_HTML);
}

void sendState() {
  struct tm t;
  char clockStr[16] = "--:--";
  if (timeValid && getLocalTime(&t, 50))
    snprintf(clockStr, sizeof clockStr, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);

  char buf[470];
  snprintf(buf, sizeof buf,
    "{\"hue\":%u,\"spread\":%u,\"env\":%u,\"secpath\":%u,\"sectrail\":%u,"
    "\"r\":%u,\"g\":%u,\"b\":%u,\"bri\":%u,"
    "\"h12\":%s,\"colon\":%u,\"speed\":%u,\"tick\":%u,\"cards\":%u,\"fw\":\"%s\",\"btc\":%ld,\"tempF\":%.1f,\"city\":\"%s\","
    "\"time\":\"%s\",\"ip\":\"%s\"}",
    cfg.hue, cfg.spread, cfg.env, cfg.secpath, cfg.sectrail,
    cfg.r, cfg.g, cfg.b, cfg.brightness,
    cfg.hour12 ? "true" : "false", cfg.colon, cfg.speed, cfg.tickMins, cfg.cards, FW_VERSION, btc.usd, btc.wxOk ? btc.tempF : 0.0f, geoCity.c_str(), clockStr,
    WiFi.localIP().toString().c_str());
  server.send(200, "application/json", buf);
}

// Every field is clamped to its own range. The page cannot send a bad value,
// but the page is not the only thing that can reach this endpoint — a stray
// brightness of 255 with mode SOLID and white set is 1.9 A.
static uint8_t argU8(const char* k, uint8_t cur, uint8_t lo, uint8_t hi) {
  if (!server.hasArg(k)) return cur;
  long v = server.arg(k).toInt();
  return (uint8_t)constrain(v, (long)lo, (long)hi);
}

void handleSet() {
  cfg.hue        = argU8("hue",    cfg.hue,    0, HUE_COUNT - 1);
  cfg.spread     = argU8("spread", cfg.spread, 0, 64);
  cfg.env        = argU8("env",    cfg.env,    0, ENV_COUNT - 1);
  cfg.secpath    = argU8("secpath",  cfg.secpath,  0, PATH_COUNT - 1);
  cfg.sectrail   = argU8("sectrail", cfg.sectrail, 0, TRAIL_COUNT - 1);
  cfg.r          = argU8("r",     cfg.r,    0, 255);
  cfg.g          = argU8("g",     cfg.g,    0, 255);
  cfg.b          = argU8("b",     cfg.b,    0, 255);
  cfg.brightness = argU8("bri",   cfg.brightness, 5, 255);
  cfg.colon      = argU8("colon", cfg.colon, 0, COLON_OFF);
  cfg.speed      = argU8("speed", cfg.speed, 1, 20);
  cfg.tickMins   = argU8("tick",  cfg.tickMins, 0, 60);
  cfg.cards      = argU8("cards", cfg.cards, 0, 3);
  if (server.hasArg("h12")) cfg.hour12 = server.arg("h12").toInt() != 0;

  FastLED.setBrightness(cfg.brightness);
  saveSettings(cfg);        // survives a power cut; the clock is a fixture
  sendState();
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  // 802.11 caps these. Over-length stores fine and then never associates, which
  // looks like a wrong password forever.
  if (ssid.isEmpty() || ssid.length() > SSID_MAX || pass.length() > PASS_MAX) {
    server.send(400, "text/plain", "ssid 1-32 chars, password up to 63");
    return;
  }

  // Every one of these can fail — NVS full, partition corrupt — and the old
  // code discarded all three return values and told the user "Saved". The
  // device then rebooted, found nothing, and returned to the portal in a loop
  // that reported success on every pass.
  bool ok = prefs.begin("wifi", false);
  if (ok) ok = prefs.putString("ssid", ssid) > 0;
  if (ok) ok = (pass.isEmpty() || prefs.putString("pass", pass) > 0);
  prefs.end();

  if (!ok) { server.send(500, "text/plain", "could not write settings to flash"); return; }

  server.send(200, "text/html", "<meta name=viewport content='width=device-width'>"
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
<p class=n>Use <b>firmware.bin</b> from .pio/build/c3supermini/ &mdash; not
firmware.factory.bin, which includes the bootloader and is for first flash over USB.</p>
<p class=n><a href="/">&larr; back to settings</a></p></div>
<script>
const f=document.getElementById('f'),p=document.getElementById('p'),s=document.getElementById('s');
f.onsubmit=e=>{e.preventDefault();const x=new XMLHttpRequest(),d=new FormData(f);
 p.hidden=false;
 x.upload.onprogress=v=>{p.value=v.loaded/v.total*100;s.textContent='Uploading '+p.value.toFixed(0)+'%'};
 x.onload=()=>{s.textContent=x.status==200?'Done — restarting. This page will not respond for ~10s.':'Failed: '+x.responseText};
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
 *  unauthenticated calls an hour per IP, and this runs at most daily. */
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
  server.on("/save", HTTP_POST, handleSave);
  server.on("/update", HTTP_GET, [] { server.send_P(200, "text/html", UPDATE_HTML); });
  server.on("/checkupdate", [] {
    bool ok = checkUpdate();
    char b[160];
    snprintf(b, sizeof b, "{\"ok\":%s,\"current\":\"%s\",\"latest\":\"%s\",\"newer\":%s}",
             ok ? "true" : "false", FW_VERSION, latestTag.c_str(),
             (ok && isNewer(latestTag, FW_VERSION)) ? "true" : "false");
    server.send(200, "application/json", b);
  });
  server.on("/doupdate", [] {
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
// own retry), so the guard is not optional — it froze the clock coming home
// from MakerNexus.
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

  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
  loadSettings(cfg);                 // defaults until the page has been used
  FastLED.setBrightness(cfg.brightness);
  // BRIGHTNESS is a bare knob. At 40 the worst case is ~180 mA; at 255 a white
  // "88:88" is 32 x 60 mA = 1.9 A through the Super Mini's VBUS trace. This
  // ceiling makes raising it safe.
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MA);
  display.setForeground(S7Color(cfg.r, cfg.g, cfg.b));
  display.setBackground(S7Color(0, 0, 0));
  display.setBackgroundMode(BG_OVERWRITE);
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

  // The background SNTP poller only refreshes hourly and the C3's RTC free-runs
  // on an uncalibrated oscillator, so pin it down periodically.
  if (millis() - lastSyncMs > RESYNC_AFTER_MS) syncTime();

  // Ticker: fetch on its own cache timer, scroll on the interval you set.
  // Guarded on timeValid so a clock that does not yet know the time never
  // wanders off to fetch a price.
  if (cfg.tickMins && timeValid && millis() - lastTickMs > cfg.tickMins * 60000UL) {
    lastTickMs = millis();
    struct tm t;
    if (getLocalTime(&t, 50)) {
      auto pump = [] { server.handleClient(); };
      if ((cfg.cards & CARD_BTC) && tickerFetch(btc))
        tickerScroll(display, leds, NUM_LEDS, btc, cfg, t, pump);
      if ((cfg.cards & CARD_TEMP) && (cfg.lat || cfg.lon) && tickerWeather(btc, cfg.lat, cfg.lon))
        tempShow(display, btc, cfg, t, pump);
    }
  }

  showTime();
  delay(200);
}
