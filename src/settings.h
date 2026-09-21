// Display settings, NVS persistence, and the colour engine.
//
// THE MODE LIST WAS THE WRONG SHAPE
//   v1 had five "modes": Solid, Rainbow, Spectrum, Chrono, Breathe. Two of them
//   were not modes at all:
//     Spectrum == Rainbow + a per-digit hue offset   (literally `+ pos*48`)
//     Breathe  == Solid   + a brightness envelope
//   So spatial spread and brightness envelope are AXES, and baking them into
//   the list made 13 of the 18 combinations unreachable — including Chrono with
//   spread, which puts the time-of-day hue as a gradient across the digits.
//
//   Three orthogonal controls now:
//       hue source   fixed | cycling | chrono
//     x spread       0..64 per-digit hue offset
//     x envelope     none | breathe
//   plus an independent seconds overlay. The colon has always been separate —
//   that was the precedent this should have followed from the start.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <FastLED.h>
#include <sys/time.h>
#include "String7Segment.h"

// Pins offered for the LED data line, per chip.
//
// The list is NOT cosmetic. Every chip has strapping pins — sampled at reset to
// choose the boot mode — and a WS2812 line idles low, so a strapping pin wired
// to one can hold the part in download mode at power-up. That failure looks
// exactly like a dead board, which is why these pins are withheld rather than
// merely warned about.
//
// Withheld everywhere: strapping pins, the USB D+/D- pair, the SPI flash bus,
// and the UART0 pair (usable here, since Serial runs over USB CDC, but they are
// what a serial adapter would want back).
//
// ⚠ One list, one owner. The web page renders whatever /api reports and has no
//   copy of its own — an allow-list duplicated into the JS would drift the first
//   time a chip was added, and the UI would offer pins the firmware refuses.
// An X-macro, so the list is written ONCE and both consumers derive from it:
// the array /api publishes, and the switch that instantiates FastLED. Writing
// the switch out separately is the obvious alternative and it is a trap — the
// two copies stay in step exactly until someone adds a chip.
#if   defined(CONFIG_IDF_TARGET_ESP32C3)
  #define PIN_XLIST  X(0) X(1) X(3) X(4) X(5) X(6) X(7) X(10)
  #define DEFAULT_DATA_PIN 4                  // strapping: 2, 8, 9
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #define PIN_XLIST  X(0) X(1) X(2) X(3) X(6) X(7) X(10) X(11) X(18) X(19)
  #define DEFAULT_DATA_PIN 2                  // strapping: 4, 5, 8, 9, 15
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
  #define PIN_XLIST  X(1) X(2) X(4) X(5) X(6) X(7) X(8) X(9) X(10) \
                     X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(21)
  #define DEFAULT_DATA_PIN 4                  // strapping: 0, 3, 45, 46; USB: 19, 20
#elif defined(CONFIG_IDF_TARGET_ESP32)
  #define PIN_XLIST  X(4) X(5) X(13) X(14) X(16) X(17) X(18) X(19) \
                     X(21) X(22) X(23) X(25) X(26) X(27) X(32) X(33)
  #define DEFAULT_DATA_PIN 4                  // strapping: 0, 2, 5, 12, 15
#else
  #error "No LED pin allow-list for this chip. Add one rather than guessing."
#endif

#define X(n) n,
static constexpr uint8_t PIN_LIST[] = { PIN_XLIST };
#undef X
static constexpr uint8_t PIN_COUNT = sizeof(PIN_LIST);

static constexpr uint8_t MAX_DIGITS       = 8;
static constexpr uint8_t SEGMENT_COUNT    = 8;
static constexpr uint8_t MAX_LEDS_PER_SEG = 8;
static constexpr uint16_t MAX_LEDS        = MAX_DIGITS * SEGMENT_COUNT * MAX_LEDS_PER_SEG;
// uint16_t, not uint8_t, and the reason is a boundary that bites silently.
// A uint8_t base addresses 0..254 with 255 spent on the sentinel, so the real
// ceiling would be 255 LEDs -- while the buffer below is MAX_LEDS (512). A
// fully-loaded 8 digits x 8 LEDs/segment would have had nowhere to put half its
// indices. Widening costs 64 bytes of RAM and the same in NVS, and it makes the
// ~770 bytes of CRGB buffer that were already allocated actually reachable.
static constexpr uint16_t SEGMENT_ABSENT   = 0xFFFF;
static constexpr uint16_t SEGMENT_BASE_MAX = MAX_LEDS - 1;
static constexpr uint8_t SEGMENT_DP_INDEX = 7;

static constexpr uint8_t DEFAULT_DIGITS       = 4;
static constexpr uint8_t DEFAULT_LEDS_PER_SEG = 1;
static constexpr uint16_t DEFAULT_DP_MASK     = 0x000F;

enum Hue    : uint8_t { HUE_FIXED = 0, HUE_CYCLE, HUE_CHRONO, HUE_COUNT };
enum Env    : uint8_t { ENV_NONE  = 0, ENV_BREATHE, ENV_COUNT };
enum Colon  : uint8_t { COLON_BLINK = 0, COLON_ON, COLON_OFF };
// Seconds overlay, decomposed the same way the colour modes were. "Comet" was
// Sweep-with-a-tail and "Fill" was Sweep-with-an-accumulating-tail, and Ring
// had a fill welded into it — three more modes that were really one axis wearing
// different hats. Path says WHERE the cursor goes, trail says WHAT IT LEAVES.
enum SecPath  : uint8_t { PATH_OFF = 0, PATH_TRACE, PATH_GHOST, PATH_ORBIT, PATH_RING, PATH_COUNT };
enum SecTrail : uint8_t { TRAIL_NONE = 0, TRAIL_COMET, TRAIL_FILL, TRAIL_COUNT };

struct Settings {
  uint8_t hue        = HUE_FIXED;
  uint8_t spread     = 0;      // 0..64, per-digit hue offset
  uint8_t env        = ENV_NONE;
  uint8_t secpath    = PATH_OFF;
  uint8_t sectrail   = TRAIL_NONE;
  uint8_t r          = 0, g = 160, b = 255;
  uint8_t brightness = 40;
  bool    hour12     = false;
  uint8_t colon      = COLON_BLINK;
  uint8_t speed      = 6;      // 1..20, rate for cycling and breathing
  uint8_t tickMins   = 0;      // 0 = ticker off, else show the cards every N min
  uint8_t cards      = 0x03;   // bitmask: 1 = bitcoin, 2 = temperature
  float   lat        = 0, lon = 0;   // 0,0 = not located yet (and no clocks live there)
  uint8_t dataPin    = DEFAULT_DATA_PIN;   // see PIN_XLIST — changing needs a reboot
  uint8_t digits      = DEFAULT_DIGITS;
  uint8_t ledsPerSeg  = DEFAULT_LEDS_PER_SEG;
  uint16_t dpMask     = DEFAULT_DP_MASK;
  uint16_t stripLen   = 0;     // 0 = derive from the segments; else the real strip length
  uint16_t segBase[MAX_DIGITS][SEGMENT_COUNT] = {};
};

inline uint16_t activeSegmentCount(const Settings& s) {
  uint16_t n = (uint16_t)s.digits * (SEGMENT_COUNT - 1);
  for (uint8_t d = 0; d < s.digits && d < MAX_DIGITS; d++)
    if (s.dpMask & (1U << d)) n++;
  return n;
}

// The strip can be LONGER than the sum of its segments. A hand-built display
// often has LEDs that belong to no segment at all -- spares, a run of strip
// carried between digits, pixels used for something else entirely. Deriving the
// length from the segments made those LEDs unaddressable, which also meant the
// Learn wizard could not probe them: it ran out of groups exactly where the
// segments ended, so a gap was invisible rather than skippable.
//
// stripLen 0 means "derive it", which is what an untouched device does.
inline uint16_t activeLedCount(const Settings& s) {
  uint16_t derived = activeSegmentCount(s) * s.ledsPerSeg;
  if (s.stripLen == 0) return derived;
  return s.stripLen < derived ? derived : s.stripLen;
}

inline void geometryReset(Settings& s) {
  uint16_t n = 0;
  for (uint8_t d = 0; d < MAX_DIGITS; d++) {
    for (uint8_t seg = 0; seg < SEGMENT_COUNT; seg++) {
      if (d >= s.digits) {
        s.segBase[d][seg] = SEGMENT_ABSENT;
      } else if (seg == SEGMENT_DP_INDEX && !(s.dpMask & (1U << d))) {
        s.segBase[d][seg] = SEGMENT_ABSENT;
      } else {
        s.segBase[d][seg] = (uint16_t)(n++ * s.ledsPerSeg);
      }
    }
  }
}

inline bool segmentWritable(const Settings& s, uint8_t pos, uint8_t seg) {
  if (pos >= s.digits || seg >= SEGMENT_COUNT) return false;
  if (seg == SEGMENT_DP_INDEX && !(s.dpMask & (1U << pos))) return false;
  return s.segBase[pos][seg] != SEGMENT_ABSENT;
}

inline bool segmentLedIndex(const Settings& s, uint8_t pos, uint8_t seg,
                            uint8_t led, uint16_t& out) {
  if (led >= s.ledsPerSeg || !segmentWritable(s, pos, seg)) return false;
  uint16_t idx = s.segBase[pos][seg] + led;
  if (idx >= activeLedCount(s) || idx >= MAX_LEDS) return false;
  out = idx;
  return true;
}

inline bool segmentLit(CRGB* leds, const Settings& s, uint8_t pos, uint8_t seg) {
  for (uint8_t led = 0; led < s.ledsPerSeg; led++) {
    uint16_t idx;
    if (segmentLedIndex(s, pos, seg, led, idx) && leds[idx]) return true;
  }
  return false;
}

inline void clearDisplay(CRGB* leds, const Settings& s) {
  fill_solid(leds, activeLedCount(s), CRGB::Black);
}

inline void renderSegments(CRGB* leds, const Settings& s, uint8_t pos,
                           uint8_t segmentMask, CRGB colour) {
  if (pos >= s.digits) return;
  for (uint8_t seg = 0; seg < SEGMENT_COUNT; seg++) {
    if (!(segmentMask & (1U << seg))) continue;
    for (uint8_t led = 0; led < s.ledsPerSeg; led++) {
      uint16_t idx;
      if (segmentLedIndex(s, pos, seg, led, idx)) leds[idx] = colour;
    }
  }
}

inline void renderChar(CRGB* leds, const Settings& s, uint8_t pos, char c, CRGB colour) {
  renderSegments(leds, s, pos, String7Segment::getPattern(c), colour);
}

inline void renderDecimalPoint(CRGB* leds, const Settings& s, uint8_t pos, CRGB colour) {
  renderSegments(leds, s, pos, SEG_DP, colour);
}

inline bool geometryProblem(const Settings& s, char* err, size_t errLen) {
  if (s.digits < 1 || s.digits > MAX_DIGITS) {
    snprintf(err, errLen, "digits must be 1..%u", MAX_DIGITS);
    return true;
  }
  if (s.ledsPerSeg < 1 || s.ledsPerSeg > MAX_LEDS_PER_SEG) {
    snprintf(err, errLen, "ledsPerSeg must be 1..%u", MAX_LEDS_PER_SEG);
    return true;
  }
  if (s.dpMask & ~((1U << MAX_DIGITS) - 1U)) {
    snprintf(err, errLen, "dpMask uses bits above digit %u", (uint8_t)(MAX_DIGITS - 1));
    return true;
  }
  if (activeLedCount(s) > MAX_LEDS) {
    snprintf(err, errLen, "geometry uses more than MAX_LEDS");
    return true;
  }

  bool used[MAX_LEDS] = {};
  uint16_t live = activeLedCount(s);
  for (uint8_t d = 0; d < MAX_DIGITS; d++) {
    for (uint8_t seg = 0; seg < SEGMENT_COUNT; seg++) {
      uint16_t base = s.segBase[d][seg];
      if (d >= s.digits) {
        if (base != SEGMENT_ABSENT) {
          snprintf(err, errLen, "segBase[%u][%u] must be %u for absent digit", d, seg, SEGMENT_ABSENT);
          return true;
        }
        continue;
      }
      if (seg == SEGMENT_DP_INDEX && !(s.dpMask & (1U << d))) {
        if (base != SEGMENT_ABSENT) {
          snprintf(err, errLen, "segBase[%u][%u] must be %u when dpMask bit is clear", d, seg, SEGMENT_ABSENT);
          return true;
        }
        continue;
      }
      if (base == SEGMENT_ABSENT) continue;
      uint16_t end = base + s.ledsPerSeg - 1;
      if (d < s.digits && end >= live) {
        snprintf(err, errLen, "segBase[%u][%u] run exceeds active LED count %u", d, seg, live);
        return true;
      }
      if (end >= MAX_LEDS) {
        snprintf(err, errLen, "segBase[%u][%u] run exceeds MAX_LEDS", d, seg);
        return true;
      }
      for (uint16_t i = base; i <= end; i++) {
        if (used[i]) {
          snprintf(err, errLen, "segBase[%u][%u] run overlaps LED %u", d, seg, i);
          return true;
        }
        used[i] = true;
      }
    }
  }
  return false;
}

inline bool packedGeometryProblem(const Settings& s, char* err, size_t errLen) {
  uint16_t groups = activeSegmentCount(s);
  if (!groups) return false;
  uint32_t lastBase = (uint32_t)(groups - 1) * s.ledsPerSeg;
  if (lastBase > SEGMENT_BASE_MAX) {
    snprintf(err, errLen, "packed geometry needs base %lu but segBase can store only 0..%u",
             (unsigned long)lastBase, SEGMENT_BASE_MAX);
    return true;
  }
  return false;
}

inline void loadGeometry(Preferences& p, Settings& s) {
  geometryReset(s);
  if (!p.isKey("digits") || !p.isKey("lps") || !p.isKey("dpmask") || !p.isKey("segbase")) return;
  if (p.getBytesLength("segbase") != sizeof(s.segBase)) return;

  Settings g = s;
  g.digits = p.getUChar("digits", g.digits);
  g.ledsPerSeg = p.getUChar("lps", g.ledsPerSeg);
  g.dpMask = p.getUShort("dpmask", g.dpMask);
  g.stripLen = p.getUShort("striplen", g.stripLen);   // absent on pre-1.1 devices -> 0 -> derive
  if (p.getBytes("segbase", g.segBase, sizeof(g.segBase)) != sizeof(g.segBase)) return;

  char err[96];
  if (geometryProblem(g, err, sizeof err)) return;
  s.digits = g.digits;
  s.ledsPerSeg = g.ledsPerSeg;
  s.dpMask = g.dpMask;
  s.stripLen = g.stripLen;
  memcpy(s.segBase, g.segBase, sizeof(s.segBase));
}

inline void loadSettings(Settings& s) {
  geometryReset(s);
  Preferences p;
  if (!p.begin("disp", true)) return;
  loadGeometry(p, s);

  // Migration. A device already in service has the old single `mode` key and no
  // axis keys; silently resetting it to defaults would be a rude way to ship an
  // internal refactor. Map the old five onto the new axes instead.
  if (!p.isKey("hue") && p.isKey("mode")) {
    switch (p.getUChar("mode", 0)) {
      case 1: s.hue = HUE_CYCLE;                      break;  // Rainbow
      case 2: s.hue = HUE_CYCLE;  s.spread = 48;      break;  // Spectrum
      case 3: s.hue = HUE_CHRONO;                     break;  // Chrono
      case 4: s.hue = HUE_FIXED;  s.env = ENV_BREATHE; break; // Breathe
      default: s.hue = HUE_FIXED;                     break;  // Solid
    }
  } else {
    s.hue    = p.getUChar("hue",    s.hue);
    s.spread = p.getUChar("spread", s.spread);
    s.env    = p.getUChar("env",    s.env);
  }
  // Old single secfx key -> path + trail, so a device in service keeps its look.
  if (!p.isKey("secpath") && p.isKey("secfx")) {
    switch (p.getUChar("secfx", 0)) {
      case 1: s.secpath = PATH_TRACE;                              break; // Sweep
      case 2: s.secpath = PATH_TRACE; s.sectrail = TRAIL_COMET;    break; // Comet
      case 3: s.secpath = PATH_TRACE; s.sectrail = TRAIL_FILL;     break; // Fill
      case 4: s.secpath = PATH_GHOST;                              break; // Ghost
      case 5: s.secpath = PATH_ORBIT;                              break; // Orbit
      case 6: s.secpath = PATH_RING;  s.sectrail = TRAIL_FILL;     break; // Ring
      default: s.secpath = PATH_OFF;                               break;
    }
  } else {
    s.secpath  = p.getUChar("secpath",  s.secpath);
    s.sectrail = p.getUChar("sectrail", s.sectrail);
  }
  s.r          = p.getUChar("r",     s.r);
  s.g          = p.getUChar("g",     s.g);
  s.b          = p.getUChar("b",     s.b);
  s.brightness = p.getUChar("bri",   s.brightness);
  s.hour12     = p.getBool ("h12",   s.hour12);
  s.colon      = p.getUChar("colon", s.colon);
  s.speed      = p.getUChar("speed", s.speed);
  s.tickMins   = p.getUChar("tick",  s.tickMins);
  s.cards      = p.getUChar("cards", s.cards);
  s.lat        = p.getFloat("lat",   s.lat);
  s.lon        = p.getFloat("lon",   s.lon);
  s.dataPin    = p.getUChar("pin",   s.dataPin);
  p.end();
}

inline bool saveSettings(const Settings& s) {
  Preferences p;
  if (!p.begin("disp", false)) return false;
  bool ok = p.putUChar("hue",    s.hue)
         && p.putUChar("spread", s.spread)
         && p.putUChar("env",    s.env)
         && p.putUChar("secpath",  s.secpath)
         && p.putUChar("sectrail", s.sectrail)
         && p.putUChar("r", s.r) && p.putUChar("g", s.g) && p.putUChar("b", s.b)
         && p.putUChar("bri",   s.brightness)
         && p.putUChar("colon", s.colon)
         && p.putUChar("speed", s.speed)
         && p.putUChar("tick",  s.tickMins)
         && p.putUChar("cards", s.cards)
         && p.putUChar("digits", s.digits)
         && p.putUChar("lps", s.ledsPerSeg)
         && p.putUShort("dpmask", s.dpMask)
         && p.putUShort("striplen", s.stripLen)
         && p.putBytes("segbase", s.segBase, sizeof(s.segBase)) == sizeof(s.segBase);
  p.putFloat("lat", s.lat); p.putFloat("lon", s.lon);
  p.putUChar("pin", s.dataPin);
  p.putBool("h12", s.hour12);              // 0 is a legal size for `false`
  p.end();
  return ok;
}

/** Seconds with sub-second resolution — tm only carries whole seconds, and an
 *  overlay that steps once a second looks broken rather than deliberate. */
inline float secondsNow(const struct tm& t) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return t.tm_sec + tv.tv_usec / 1000000.0f;
}

/** Colour for one digit this frame: hue source, then spread, then envelope. */
inline CRGB colourFor(const Settings& s, uint8_t pos, const struct tm& t, uint32_t ms) {
  uint8_t h;
  bool useRGB = false;

  switch (s.hue) {
    case HUE_CYCLE:
      h = (ms / (21 - s.speed) / 8) & 0xFF;
      break;
    case HUE_CHRONO: {
      // Hue IS the time of day: cold overnight, warm at noon, violet by
      // evening. You end up reading the hour off the colour before the digits.
      uint32_t secs = t.tm_hour * 3600UL + t.tm_min * 60UL + t.tm_sec;
      h = (uint8_t)((secs * 255UL) / 86400UL);
      break;
    }
    default:
      useRGB = true; h = 0;
      break;
  }

  // Spread applies to any hue source, which is the whole point of the refactor:
  // Chrono + spread was unreachable before and is the nicest thing in here.
  CRGB c;
  if (useRGB && s.spread == 0) {
    c = CRGB(s.r, s.g, s.b);
  } else if (useRGB) {
    // A fixed colour has no hue to walk, so spread shifts it around the wheel
    // from wherever you put it rather than ignoring the setting.
    CHSV base = rgb2hsv_approximate(CRGB(s.r, s.g, s.b));
    c = CHSV(base.h + pos * s.spread / 4, base.s, base.v);
  } else {
    c = CHSV((h + pos * s.spread / 4) & 0xFF, s.hue == HUE_CHRONO ? 220 : 255, 255);
  }

  if (s.env == ENV_BREATHE) {
    // cubicwave8, not a triangle: the eye reads linear ramps as jerky at the
    // turnarounds and this eases both ends.
    uint8_t v = 40 + scale8(cubicwave8((ms / (21 - s.speed) / 4) & 0xFF), 215);
    c.nscale8_video(v);
  }
  return c;
}

/**
 * Seconds overlay = PATH x TRAIL.
 *
 * PATH — where the cursor goes
 *   TRACE  only the lit segments, so it walks the numerals themselves. The
 *          panel is wired A,B,C,D,E,F,G,DP, which is clockwise from the top,
 *          so this traces each digit clockwise for free. Path length changes
 *          with the time: "11:11" is short, "08:88" long.
 *   GHOST  all 28 non-DP segments, lit or not. Constant length, so the beat
 *          never depends on what the time happens to light up.
 *   ORBIT  four cursors, one per digit, each round its own outer ring with a
 *          one-step offset so they read as a wave. Laps every 6 s.
 *   RING   the perimeter of the whole display: A across the top, B/C down the
 *          right, D back along the bottom, E/F up the left. Twelve segments —
 *          and 60/12 is exactly 5, so this one is minute-locked and its
 *          position is a genuine second hand, not decoration.
 *
 * TRAIL — what it leaves behind
 *   NONE   a single cursor
 *   COMET  a short fading tail
 *   FILL   everything already passed stays faintly lit, so it reads as a dial
 *
 * Every path except RING advances one step per second, which is what keeps the
 * beat. RING advances by minute fraction and pulses in place instead.
 */
struct SegmentRef {
  uint8_t digit;
  uint8_t seg;
};

inline void applyLane(CRGB* leds, const SegmentRef* idx, uint8_t cnt, uint8_t head,
                      const Settings& s, uint8_t punch, const CRGB& tint) {
  auto mark = [&](SegmentRef ref, uint8_t amt) {
    for (uint8_t led = 0; led < s.ledsPerSeg; led++) {
      uint16_t i;
      if (!segmentLedIndex(s, ref.digit, ref.seg, led, i)) continue;
      if (leds[i]) {
        leds[i] = blend(leds[i], CRGB::White, amt);
      } else {
        // Dark segment gets a dim ghost in the display's own colour. Scaled hard
        // — a bright ghost turns "12:34" into an unreadable "88:88".
        CRGB g = tint;
        g.nscale8_video(scale8(amt, 70));
        leds[i] = g;
      }
    }
  };

  if (s.sectrail == TRAIL_COMET) {
    const uint8_t TAIL = 4;
    for (uint8_t k = 1; k < TAIL && k < cnt; k++)
      mark(idx[(head + cnt - k) % cnt], (uint8_t)((uint16_t)punch * (TAIL - k) / (TAIL * 2)));
  } else if (s.sectrail == TRAIL_FILL) {
    for (uint8_t k = 0; k < head && k < cnt; k++) mark(idx[k], 55);
  }
  mark(idx[head % cnt], punch);       // head last, so it always wins
}

inline void applySeconds(CRGB* leds, uint16_t n, float secf, const Settings& s, const CRGB& tint) {
  if (s.secpath == PATH_OFF || n == 0) return;
  uint8_t nd = s.digits;
  if (nd == 0) return;

  uint16_t sec  = (uint16_t)secf;
  float    frac = secf - sec;
  // The pulse decays across the second so the beat is something you see ARRIVE,
  // not a cursor that merely happens to be somewhere. Floor of 60 keeps it
  // legible right up to the next tick.
  uint8_t punch = 60 + (uint8_t)((1.0f - frac) * 195.0f);

  SegmentRef idx[MAX_DIGITS * 7];
  uint8_t cnt = 0;
  auto push = [&](uint8_t d, uint8_t seg) {
    if (cnt < (uint8_t)(sizeof idx / sizeof idx[0]) && segmentWritable(s, d, seg))
      idx[cnt++] = { d, seg };
  };

  switch (s.secpath) {
    case PATH_TRACE:
      for (uint8_t d = 0; d < nd; d++)
        for (uint8_t seg = 0; seg < SEGMENT_DP_INDEX; seg++)
          if (segmentLit(leds, s, d, seg)) push(d, seg);
      if (cnt) applyLane(leds, idx, cnt, sec % cnt, s, punch, tint);
      return;

    case PATH_GHOST:
      for (uint8_t d = 0; d < nd; d++)
        for (uint8_t seg = 0; seg < SEGMENT_DP_INDEX; seg++) push(d, seg);
      if (cnt) applyLane(leds, idx, cnt, sec % cnt, s, punch, tint);
      return;

    case PATH_ORBIT:
      // Four independent lanes — the trail applies within each digit's ring.
      for (uint8_t d = 0; d < nd; d++) {
        SegmentRef ring[6];
        uint8_t ringCnt = 0;
        for (uint8_t seg = 0; seg < 6; seg++)
          if (segmentWritable(s, d, seg)) ring[ringCnt++] = { d, seg };
        if (ringCnt) applyLane(leds, ring, ringCnt, (sec + d) % ringCnt, s, punch, tint);
      }
      return;

    case PATH_RING: {
      for (uint8_t d = 0; d < nd; d++)     push(d, 0);                   // A, L->R
      push(nd - 1, 1);                                                   // B
      push(nd - 1, 2);                                                   // C
      for (int8_t d = nd - 1; d >= 0; d--) push(d, 3);                   // D, R->L
      push(0, 4);                                                        // E
      push(0, 5);                                                        // F
      // Minute-locked, not per-second: with 12 markers at 5 s each the cursor
      // position IS the second hand.
      if (cnt) applyLane(leds, idx, cnt, (uint8_t)((sec % 60) * cnt / 60), s, punch, tint);
      return;
    }

    default:
      return;   // Old or corrupt NVS enum values disable the overlay.
  }
}

inline const char* hueName(uint8_t h) {
  return h == HUE_CYCLE ? "Cycle" : h == HUE_CHRONO ? "Chrono" : "Fixed";
}
