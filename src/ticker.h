// Bitcoin price ticker: fetch, cache, and scroll across the four digits.
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
//   The fetch is cached and every error path leaves the last good price in
//   place. A ticker that cannot reach CoinGecko must never take the clock down
//   with it — the clock is the job, this is the party trick.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "String7Segment.h"
#include "settings.h"

#define TICKER_URL   "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd"
// Plain HTTP on purpose: ip-api's free tier is HTTP-only, the payload is not
// sensitive, and skipping a TLS handshake for one call at boot is worth it.
#define GEO_URL      "http://ip-api.com/json/?fields=status,city,lat,lon"
#define WX_URL       "https://api.open-meteo.com/v1/forecast?current=temperature_2m&temperature_unit=fahrenheit&latitude="
#define WX_CACHE     600000UL     // weather moves slowly; 10 min is plenty
#define TICKER_CACHE 120000UL     // don't hammer the free API: 2 min minimum
#define TICKER_STEP_MS 430        // per frame; 300 was brisk enough to be work to read
#define TICKER_PASSES  2          // a glance that arrives mid-scroll still gets it

struct Ticker {
  long     usd       = 0;         // 0 = never successfully fetched
  uint32_t fetchedMs = 0;
  bool     ok        = false;

  float    tempF     = 0;
  uint32_t wxMs      = 0;
  bool     wxOk      = false;
};

// Cards the ticker can show, as a bitmask so adding a third costs one bit
// rather than another mode enum.
#define CARD_BTC   0x01
#define CARD_TEMP  0x02

/** One small GET into a String. Every caller treats failure as "keep the last
 *  good value", so this returns empty rather than throwing a wobbly. */
inline String httpGet(const char* url, bool secure) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  bool begun;
  WiFiClientSecure tls;
  if (secure) { tls.setInsecure(); tls.setTimeout(8); begun = http.begin(tls, url); }
  else        { begun = http.begin(url); }
  if (!begun) return "";
  http.setTimeout(8000);
  int code = http.GET();
  String body = (code == 200) ? http.getString() : String("");
  if (code != 200) Serial.printf("[NET] %s -> %d\n", url, code);
  http.end();
  return body;
}

/** Locate by public IP. Called once and cached to NVS — the clock only moves
 *  house occasionally, and the answer is good to a few miles which is well
 *  inside the resolution of "what is the temperature outside". */
inline bool geoLocate(float& lat, float& lon, String& city) {
  String b = httpGet(GEO_URL, false);
  if (b.indexOf("\"status\":\"success\"") < 0) return false;
  int la = b.indexOf("\"lat\":"), lo = b.indexOf("\"lon\":"), ci = b.indexOf("\"city\":\"");
  if (la < 0 || lo < 0) return false;
  lat = b.substring(la + 6).toFloat();
  lon = b.substring(lo + 6).toFloat();
  if (ci >= 0) { int e = b.indexOf('"', ci + 8); city = b.substring(ci + 8, e); }
  Serial.printf("[GEO] %s  %.4f, %.4f\n", city.c_str(), lat, lon);
  return true;
}

inline bool tickerWeather(Ticker& tk, float lat, float lon) {
  if (tk.wxOk && millis() - tk.wxMs < WX_CACHE) return true;
  char url[200];
  snprintf(url, sizeof url, "%s%.4f&longitude=%.4f", WX_URL, lat, lon);
  String b = httpGet(url, true);
  int k = b.indexOf("\"temperature_2m\":");
  // The key appears twice — once in current_units as a string, once in current
  // as the number. lastIndexOf gets the value, indexOf would get "°F".
  k = b.lastIndexOf("\"temperature_2m\":");
  if (k < 0) return false;
  tk.tempF = b.substring(k + 17).toFloat();   // "temperature_2m": is 17 chars
  tk.wxOk = true; tk.wxMs = millis();
  Serial.printf("[WX] %.1f F\n", tk.tempF);
  return true;
}

/** Temperature needs no scroll: "%3dF" is exactly four characters, so -9F
 *  through 999F all fit the panel as-is. Held, not scrolled. */
inline void tempShow(String7Segment& display, const Ticker& tk, const Settings& s,
                     const struct tm& t, void (*pump)()) {
  if (!tk.wxOk) return;
  char buf[8];
  snprintf(buf, sizeof buf, "%3dF", (int)lroundf(tk.tempF));
  display.clear();
  for (uint8_t i = 0; i < 4; i++) {
    if (buf[i] == ' ') continue;
    CRGB col = colourFor(s, i, t, millis());
    display.setForeground(S7Color(col.r, col.g, col.b));
    display.showChar(buf[i], i);
  }
  FastLED.show();
  uint32_t t0 = millis();
  while (millis() - t0 < 3500) { if (pump) pump(); delay(5); }
}

inline bool tickerFetch(Ticker& tk) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (tk.ok && millis() - tk.fetchedMs < TICKER_CACHE) return true;   // still fresh

  WiFiClientSecure client;
  // No cert pinning. This is a public price feed on a decorative display; the
  // worst case of a spoofed response is a wrong number on a clock. Carrying a
  // root bundle that expires would be the bigger liability.
  client.setInsecure();
  client.setTimeout(8);

  HTTPClient http;
  if (!http.begin(client, TICKER_URL)) return false;
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("[BTC] http %d\n", code); return false; }

  String body = http.getString();
  http.end();

  // {"bitcoin":{"usd":81552}} — a whole JSON parser for one integer would cost
  // more flash than the rest of this file.
  int k = body.indexOf("\"usd\":");
  if (k < 0) { Serial.println("[BTC] no usd field"); return false; }
  long v = body.substring(k + 6).toInt();
  if (v <= 0) { Serial.println("[BTC] unparseable"); return false; }

  tk.usd = v;
  tk.ok = true;
  tk.fetchedMs = millis();
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
inline void tickerScroll(String7Segment& display, CRGB* leds, uint16_t n,
                         const Ticker& tk, const Settings& s,
                         const struct tm& t, void (*pump)()) {
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
  for (uint8_t pass = 0; pass < TICKER_PASSES; pass++) {
    for (uint16_t off = 0; off + W <= msg.length(); off++) {
      display.clear();
      for (uint8_t i = 0; i < W; i++) {
        uint16_t src = off + i;
        char c = msg[src];
        CRGB col = colourFor(s, i, t, millis());
        display.setForeground(S7Color(col.r, col.g, col.b));
        if (c != ' ') display.showChar(c, i);
        // The separator rides on its digit, so it scrolls with the number
        // instead of sitting at a fixed place on the panel.
        if (dpAfter[src]) display.setDecimalPoint(i, true);
      }
      FastLED.show();
      uint32_t t0 = millis();
      while (millis() - t0 < TICKER_STEP_MS) { if (pump) pump(); delay(5); }
    }
  }
}
