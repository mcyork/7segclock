// Network clients: one small HTTPS GET, the Bitcoin ticker, the weather card, and
// IP geolocation.
//
// WHY IT SCROLLS AT ALL
//   BTC is five digits ($81,552 at time of writing) and the panel has four, so
//   something has to give. The alternatives were worse: "81.5" throws away the
//   hundreds, and "8155" is a number that is not the price. Scrolling is the
//   only option that shows the actual figure.
//
// WHY IT SAYS "bC" AND NOT "btc"
//   The segment font has no 't'. Nor S, G, I, K, M, Q, V, W, X or Z. "btc"
//   would render as "b c" with a hole in it. 'b' and 'C' both exist, so the
//   label is bC — checked against getPattern(), not assumed.
//
// NETWORK FAILURE IS NOT A CLOCK FAILURE
//   Every fetch is cached and every error path leaves the last good value in
//   place. A failed fetch also arms a back-off, so a dead WAN costs the clock one
//   8 s stall every five minutes rather than one every tick. The clock is the
//   job; this is the party trick.
//
// TLS IS VERIFIED (1.2.0)
//   Every HTTPS client attaches the Mozilla root bundle that ESP-IDF already
//   links into libmbedtls, so nothing here trusts an unverified server. That
//   matters for the firmware download above all; the price and the weather get
//   it for free. The bundle carries no clock dependency in this build
//   (CONFIG_MBEDTLS_HAVE_TIME_DATE is off), so it works before NTP has landed.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "settings.h"

#define TICKER_URL   "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd"
// Plain HTTP on purpose: ip-api's free tier is HTTP-only, the payload is not
// sensitive, and a wrong city costs a wrong temperature card, nothing more.
#define GEO_URL      "http://ip-api.com/json/?fields=status,city,lat,lon"
#define WX_URL       "https://api.open-meteo.com/v1/forecast?current=temperature_2m&temperature_unit=fahrenheit&latitude="
#define WX_CACHE       600000UL   // weather moves slowly; 10 min is plenty
#define TICKER_CACHE   120000UL   // don't hammer the free API: 2 min minimum
#define NET_BACKOFF_MS 300000UL   // after a failure, leave that endpoint alone for 5 min
#define TICKER_STEP_MS 430        // per frame; 300 was brisk enough to be work to read
#define TICKER_PASSES  2          // a glance that arrives mid-scroll still gets it

struct Ticker {
  long     usd       = 0;         // 0 = never successfully fetched
  uint32_t fetchedMs = 0;
  uint32_t failMs    = 0;         // last failed price fetch, for the back-off
  bool     ok        = false;

  float    tempF     = 0;
  uint32_t wxMs      = 0;
  uint32_t wxFailMs  = 0;
  bool     wxOk      = false;
};

// Cards the ticker can show, as a bitmask so adding a third costs one bit
// rather than another mode enum.
#define CARD_BTC   0x01
#define CARD_TEMP  0x02

// The root bundle ESP-IDF embeds in libmbedtls (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE).
// esp_crt_bundle_attach() would use it by default; NetworkClientSecure only exposes
// the attach path through setCACertBundle(), which wants the bytes, so hand it the
// same bytes it would have used anyway.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

inline void trustRootBundle(WiFiClientSecure& c) {
  c.setCACertBundle(x509_crt_bundle_start, (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
}

/** One small GET into a String. Every caller treats failure as "keep the last
 *  good value", so this returns empty rather than throwing a wobbly. The HTTP
 *  status (or 0 for no connection / TLS failure) comes back through `code` so the
 *  caller can tell "no such release" (404) from "rate limited" (403) from "offline". */
inline String httpGet(const char* url, bool secure, int* code = nullptr) {
  if (code) *code = 0;
  if (WiFi.status() != WL_CONNECTED) return "";
  // Declared BEFORE the HTTPClient so it outlives it: HTTPClient's destructor
  // still touches the client it was given, and the previous ordering destroyed
  // the TLS client first.
  WiFiClientSecure tls;
  WiFiClient plain;
  HTTPClient http;
  bool begun;
  if (secure) { trustRootBundle(tls); begun = http.begin(tls, url); }
  else        { begun = http.begin(plain, url); }
  if (!begun) return "";
  // Both in milliseconds, both on HTTPClient, which propagates them to the client
  // it was handed. (setTimeout on the client itself is ambiguous between Stream's
  // milliseconds and the socket's seconds, so it is not used here.)
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  int status = http.GET();
  if (code) *code = status > 0 ? status : 0;
  String body = (status == 200) ? http.getString() : String("");
  if (status != 200) Serial.printf("[NET] %s -> %d\n", url, status);
  http.end();
  return body;
}

/** Locate by public IP. The caller stores lat/lon/city in Settings — the clock
 *  only moves house occasionally, and the answer is good to a few miles which is
 *  well inside the resolution of "what is the temperature outside". */
inline bool geoLocate(float& lat, float& lon, String& city) {
  String b = httpGet(GEO_URL, false);
  if (b.indexOf("\"status\":\"success\"") < 0) return false;
  int la = b.indexOf("\"lat\":"), lo = b.indexOf("\"lon\":"), ci = b.indexOf("\"city\":\"");
  if (la < 0 || lo < 0) return false;
  lat = b.substring(la + 6).toFloat();
  lon = b.substring(lo + 6).toFloat();
  if (ci >= 0) { int e = b.indexOf('"', ci + 8); if (e > ci) city = b.substring(ci + 8, e); }
  Serial.printf("[GEO] %s  %.4f, %.4f\n", city.c_str(), lat, lon);
  return true;
}

inline bool tickerWeather(Ticker& tk, float lat, float lon) {
  if (tk.wxOk && millis() - tk.wxMs < WX_CACHE) return true;
  if (tk.wxFailMs && millis() - tk.wxFailMs < NET_BACKOFF_MS) return tk.wxOk;   // backing off
  char url[200];
  snprintf(url, sizeof url, "%s%.4f&longitude=%.4f", WX_URL, lat, lon);
  String b = httpGet(url, true);
  // The key appears twice — once in current_units as a string, once in current
  // as the number. lastIndexOf gets the value, indexOf would get "°F".
  int k = b.lastIndexOf("\"temperature_2m\":");
  if (k < 0) { tk.wxFailMs = millis(); return tk.wxOk; }
  tk.tempF = b.substring(k + 17).toFloat();   // "temperature_2m": is 17 chars
  tk.wxOk = true; tk.wxMs = millis(); tk.wxFailMs = 0;
  Serial.printf("[WX] %.1f F\n", tk.tempF);
  return true;
}

/** Temperature needs no scroll: "%3dF" is exactly four characters, so -9F
 *  through 999F all fit the panel as-is. Held, not scrolled. */
inline void tempShow(CRGB* leds, const Ticker& tk, const Settings& s,
                     const struct tm& t, bool (*pump)()) {
  if (!tk.wxOk) return;
  char buf[8];
  snprintf(buf, sizeof buf, "%3dF", (int)lroundf(tk.tempF));
  clearDisplay(leds, s);
  uint8_t w = min<uint8_t>(4, s.digits);
  for (uint8_t i = 0; i < w; i++) {
    if (buf[i] == ' ') continue;
    CRGB col = colourFor(s, i, t, millis());
    renderChar(leds, s, i, buf[i], col);
  }
  FastLED.show();
  uint32_t t0 = millis();
  while (millis() - t0 < 3500) { if (pump && pump()) return; delay(5); }
}

inline bool tickerFetch(Ticker& tk) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (tk.ok && millis() - tk.fetchedMs < TICKER_CACHE) return true;   // still fresh
  if (tk.failMs && millis() - tk.failMs < NET_BACKOFF_MS) return tk.ok;   // backing off

  String body = httpGet(TICKER_URL, true);
  if (body.isEmpty()) { tk.failMs = millis(); return tk.ok; }

  // {"bitcoin":{"usd":81552}} — a whole JSON parser for one integer would cost
  // more flash than the rest of this file.
  int k = body.indexOf("\"usd\":");
  if (k < 0) { Serial.println("[BTC] no usd field"); tk.failMs = millis(); return tk.ok; }
  long v = body.substring(k + 6).toInt();
  if (v <= 0) { Serial.println("[BTC] unparseable"); tk.failMs = millis(); return tk.ok; }

  tk.usd = v;
  tk.ok = true;
  tk.fetchedMs = millis();
  tk.failMs = 0;
  Serial.printf("[BTC] $%ld\n", v);
  return true;
}

/**
 * Scroll the price once, right to left.
 *
 * Blocking for about four seconds, deliberately: making it stateful would mean
 * threading a scroll cursor through the main loop and the display would have to
 * know whether it was showing a clock or a ticker at every frame. The web
 * server is pumped inside the loop so the page stays responsive, and a clock
 * that pauses for four seconds twice an hour is not a clock anyone notices.
 */
inline void tickerScroll(CRGB* leds, const Ticker& tk, const Settings& s,
                         const struct tm& t, bool (*pump)()) {
  if (!tk.ok) return;

  // Thousands separator as the DECIMAL POINT of the digit before it, not as a
  // character cell. A seven-segment digit has a dot and no comma, and spending
  // a whole digit of a four-digit window on punctuation would be absurd —
  // "81.595" reads as eighty-one thousand to anyone, and it costs nothing.
  char digits[16];
  snprintf(digits, sizeof digits, "%ld", tk.usd);
  String msg = "    bC ";
  bool   dpAfter[40] = {false};
  uint8_t len = strlen(digits);
  for (uint8_t k = 0; k < len; k++) {
    uint8_t idx = msg.length();
    msg += digits[k];
    uint8_t fromRight = len - 1 - k;            // 0 = units
    if (fromRight > 0 && fromRight % 3 == 0) dpAfter[idx] = true;
  }
  msg += "    ";

  const uint8_t W = 4;
  uint8_t shown = min<uint8_t>(W, s.digits);
  for (uint8_t pass = 0; pass < TICKER_PASSES; pass++) {
    for (uint16_t off = 0; off + W <= msg.length(); off++) {
      clearDisplay(leds, s);
      for (uint8_t i = 0; i < shown; i++) {
        uint16_t src = off + i;
        char c = msg[src];
        CRGB col = colourFor(s, i, t, millis());
        if (c != ' ') renderChar(leds, s, i, c, col);
        // The separator rides on its digit, so it scrolls with the number
        // instead of sitting at a fixed place on the panel.
        if (dpAfter[src]) renderDecimalPoint(leds, s, i, col);
      }
      FastLED.show();
      uint32_t t0 = millis();
      while (millis() - t0 < TICKER_STEP_MS) { if (pump && pump()) return; delay(5); }
    }
  }
}
