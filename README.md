# 7segclock

An NTP clock on four WS2812 seven-segment digits, driven by an ESP32-C3 Super Mini.

**[Install it from the browser →](https://mcyork.github.io/7segclock/)** (Chrome or Edge)

## What it does

- Joins wifi through a captive setup portal on first boot; falls back to that
  portal if it ever cannot connect, and keeps retrying the saved network in the
  background so the portal is never a dead end.
- Gets the time by NTP with a POSIX timezone string, so DST rolls on its own.
- Serves a settings page at `mini7seg.local`, and takes firmware uploads at
  `mini7seg.local/update`.

## The display

Colour is three orthogonal controls rather than a list of modes, because two of
the original five "modes" turned out to be the same axis wearing a hat —
*Spectrum* was *Rainbow* plus a per-digit hue offset, and *Breathe* was *Solid*
plus a brightness envelope:

| axis | options |
|---|---|
| Hue source | Fixed · Cycle · **Chrono** |
| Spread | 0–64, per-digit hue offset |
| Envelope | none · Breathe |

**Chrono** maps hue to the time of day — cold overnight, warm at noon, violet by
evening. After a few days you read the hour off the colour before the digits.

The seconds indicator decomposes the same way, into a **path** and a **trail**:

| path | |
|---|---|
| Trace | walks only the lit segments, tracing each numeral clockwise |
| Ghost | walks all 28 segments, so the beat never depends on the time |
| Orbit | four cursors, one per digit, laps every 6 s |
| **Ring** | the perimeter of the whole display — 12 segments, and 60/12 = 5, so it laps once a **minute** and its position is a real second hand |

Trail is `none · comet · fill`, independently.

## Ticker

Every N minutes it can scroll the Bitcoin price (`bC 81.595`, using the decimal
point as the thousands separator, since a seven-segment digit has a dot and no
comma) and show the outside temperature (`54F`). It finds its own location from
its public IP on first connect. No API keys anywhere.

## Hardware

Three wires from the C3, all on one edge:

| C3 | panel |
|---|---|
| `5V` | `H1 V` |
| `G` | `H1 G` |
| `GPIO4` | `H1 DI` |

GPIO4 is deliberate: on the C3, GPIO2/8/9 are strapping pins sampled at reset,
and a WS2812 line idles low — tying a strapping pin to one can put the chip in
download mode at power-up, which presents as a dead board.

⚠️ WS2812 wants V_IH = 3.5 V and a C3 pin gives 3.3 V. It usually works, which
is what makes it awkward: the failure is intermittent and tracks temperature and
lead length. Fixes, cheapest first — a series Schottky in the LED 5 V feed, a
74AHCT125, or a sacrificial first pixel.

## Updating

A clock already on your network updates itself: open `mini7seg.local`, press
**Check for updates** under Firmware. It asks GitHub for the newest release tag,
compares it to what it is running, and pulls
`releases/latest/download/firmware.bin` if there is something newer.

The browser installer at [mcyork.github.io/7segclock](https://mcyork.github.io/7segclock/)
is for a *new* device, or one that will not boot — those need a cable and a full
factory image, which a running clock cannot install on itself.

### Why the two binaries live in different places

| file | where | why |
|---|---|---|
| `firmware.bin` | GitHub Releases | the device fetches it directly; an ESP32 is not a browser, so CORS does not apply |
| `firmware.factory.bin` | `docs/` in this repo | ESP Web Tools fetches it **from the browser**, and GitHub release assets send no `access-control-allow-origin` header — a cross-origin fetch is blocked, so this one has to be same-origin with the install page |

That asymmetry is not tidiness, it is the only arrangement that works.

## Build

```
pio run                                        # build
pio run -t upload                              # flash over USB
pio run -t upload --upload-port mini7seg.local # OTA
```

The segment library is [mcyork/mini7seg](https://github.com/mcyork/mini7seg),
pulled in by `lib_deps`.

## Licence

MIT
