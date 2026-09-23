# 7segclock

An NTP clock on four WS2812 seven-segment digits, driven by an ESP32-C3 Super Mini.

**[Install it from the browser →](https://mcyork.github.io/7segclock/)**

Chrome or Edge on a desktop. The installer uses Web Serial, which Safari and
Firefox do not implement — on those the install button does not appear at all,
rather than appearing and failing.

## What it does

- Joins wifi through a captive setup portal on first boot. If it later cannot
  connect it keeps showing the time, retries in the background, and only after
  a few minutes raises the portal — which keeps retrying too, so it is never a
  dead end and never a reflex.
- Gets the time by NTP with a POSIX timezone rule you pick on the page, so DST
  rolls on its own; the page shows how long ago the last real sync landed.
- Serves a settings page and a wiring editor at `mini7seg.local` (or the name
  you give it), takes firmware uploads at `/update`, and updates itself from
  this repo's releases.

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

### The data pin is settable

`GPIO4` is only the default. The settings page has a **Hardware** section
listing every pin this firmware will drive, so you can solder to whatever is
convenient on your board and select it afterwards. It takes effect on restart,
and the page offers you the restart button once you pick one.

The list is shorter than the pin count, deliberately. Strapping pins — sampled
at reset to choose the boot mode — are withheld rather than warned about,
because a WS2812 line idles low, so a strapping pin wired to one can hold the
part in download mode at power-up. That presents as a board that is simply
dead, which is a miserable thing to debug. On the C3 those are GPIO2, 8 and 9.
The USB D+/D− pair, the SPI flash bus and UART0 are withheld for the same
reason: the failure is silent.

The list lives in exactly one place, `src/settings.h`:

```c
#define PIN_XLIST  X(0) X(1) X(3) X(4) X(5) X(6) X(7) X(10)
```

An X-macro, because two consumers derive from it — the array `/api` publishes
and the `switch` that instantiates FastLED. **FastLED takes the pin as a
template parameter, not an argument**, since the clockless driver's bit timing
is generated at compile time. So "settable at runtime" really means one driver
instantiated per candidate pin and a `switch` at boot, which is why the change
needs a restart, and why the page renders whatever the device reports instead of
carrying its own copy of the list.

⚠️ WS2812 wants V_IH = 3.5 V and a C3 pin gives 3.3 V. It usually works, which
is what makes it awkward: the failure is intermittent and tracks temperature and
lead length. Fixes, cheapest first — a series Schottky in the LED 5 V feed, a
74AHCT125, or a sacrificial first pixel.

## Which ESP32s this runs on

The source is portable; the **binary is not**. ESP32 parts come in two
instruction sets, and a build for one cannot run on the other under any
circumstances — it is a recompile, not a config flag:

| ISA | parts |
|---|---|
| RISC-V | **C3**, C6, C2, H2 |
| Xtensa | ESP32 classic, S2, S3 |

The firmware itself needs almost nothing: wifi, one GPIO, and about 1.4 MB of
app. Built unmodified against three other targets, all clean:

| target | flash used (of 1.92 MB OTA slot) | status |
|---|---|---|
| ESP32-C3 | 69.7% | **shipped and tested on hardware** |
| ESP32-C6 | 71.9% | compiles; untested on silicon |
| ESP32-S3 | 78.3% | compiles; untested on silicon |
| ESP32 classic | fails to build | see below |

So the browser installer can gain other chips cheaply when there is hardware to
verify them on: one more PlatformIO env, one more `builds` entry in
`manifest.json`, and ESP Web Tools picks the right image by reading the chip ID
off the board. Only the C3 image is offered today because it is the only one
that has been run.

### Why the classic ESP32 is a different class, not just another target

It fails on `ARDUINO_USB_CDC_ON_BOOT`, and that is the whole story rather than a
build detail. The classic ESP32 and S2 have no USB-Serial-JTAG peripheral, so
boards carry a CH340 or CP2102 bridge — which means a driver install on some
machines, and the "plug it in and flash it from a web page" story is gone. The
C3/C6/S3 enumerate as USB devices on their own.

### Is the C3 Super Mini a good class of machine for this?

Yes, and for reasons that are specific to this build rather than general
enthusiasm:

- **Native USB.** No bridge chip, no drivers, so one-click browser flashing
  actually works for someone who has never installed a toolchain.
- **It fits behind the panel.** 22.5 × 18 mm is what lets the enclosure be as
  shallow as it is; an S3 devkit would set the case depth instead.
- **4 MB flash** takes two 1.92 MB OTA slots with ~30% headroom, so the clock
  can update itself.
- **~$2.** It is the cheapest part that does all of the above.

What you give up is GPIO count and the second core, neither of which this uses,
and a slightly awkward 3.3 V against the WS2812 threshold — which the S3 shares
anyway. The C6 is the natural successor if WiFi 6 or Thread ever matters here;
today it costs more and buys nothing. Adding chips beyond that is not obviously
worth it: the marginal one is a build env and a manifest entry, but each also
needs its own verified pin allow-list and a board on the bench.

## Updating

A clock already on your network updates itself: open `mini7seg.local`, press
**Check for updates** under Firmware. It asks GitHub for the newest release tag
and compares it to what it is running. **Install** then asks again, refuses
unless the release is strictly newer, and downloads that exact tag's
`firmware.bin` over verified TLS (1.2.1 and later; 1.0.0 and 1.1.0 devices pull
`releases/latest/download/firmware.bin` without checking, which is why the
release contract below never changes).

**If your clock runs 1.2.0:** its self-update cannot reach GitHub's asset CDN
(the CDN's certificate chain ends at a root that firmware does not trust). Open
`/update` and upload `firmware.bin` from the newest release by hand; every
release from 1.2.1 on updates itself.

The browser installer at [mcyork.github.io/7segclock](https://mcyork.github.io/7segclock/)
is for a *new* device, or one that will not boot — those need a cable and a full
factory image, which a running clock cannot install on itself.

### Why the two binaries live in different places

| file | where | why |
|---|---|---|
| `firmware.bin` | GitHub Releases | the device fetches it directly; an ESP32 is not a browser, so CORS does not apply |
| `firmware.factory.bin` | `docs/` in this repo | ESP Web Tools fetches it **from the browser**, and GitHub release assets send no `access-control-allow-origin` header — a cross-origin fetch is blocked, so this one has to be same-origin with the install page |

That asymmetry is not tidiness, it is the only arrangement that works.

## Timezone, name, network

Since 1.2.0 these are settings, not compile-time constants. The settings page
has a **Timezone** picker (common zones, or any POSIX rule such as
`CET-1CEST,M3.5.0,M10.5.0/3`) that applies live, so daylight saving rolls on its
own; a **Device name** (default `mini7seg`) that becomes the hostname, the mDNS
name, the ArduinoOTA name and the stem of the setup network, so two clocks can
share a LAN; a **Network** section to move the clock to another WiFi without
waiting for the portal; and a **Factory reset**. A device updated from 1.1.0
comes up with exactly the values it was compiled with.

## Trust model

Every route that changes state requires `POST` and the header `X-7seg: 1`. A
web page on another origin can make your browser send a `GET` or a form `POST`
to the clock's LAN address, but it cannot add a custom header without a CORS
preflight, which the clock never answers — so a stray `<img src=...>` cannot
change the pin or wipe a wiring map any more. There is still **no password**:
anyone on your network who opens the page can use everything on it, including
`/update` and the self-update trigger, and ArduinoOTA accepts any host on the
LAN. The setup access point uses a fixed, published password. Run the clock on
a network you trust.

Firmware downloads verify the server certificate against the Mozilla root
bundle ESP-IDF ships; the self-update only installs a release that is strictly
newer than what is running, and downloads the exact tag it verified. A freshly
installed image has to run for a minute and be reachable before the bootloader
is told to keep it; if it never gets there, the next boot returns to the
previous image.

## Repos

| repo | what lives there |
|---|---|
| **mcyork/7segclock** (this one) | the firmware, the browser installer (`docs/`), and every release — **canonical** |
| [mcyork/mini7seg](https://github.com/mcyork/mini7seg) | the `String7Segment` Arduino library and the enclosure and printed parts |

To change the library and the firmware together, clone the two repos side by
side and build with `pio run -e dev`: that environment takes the library from
`../mini7seg` through a symlink instead of the pinned git commit. Releases are
always built from `c3supermini`, never from `dev`.

## Release contract

A clock in the field running any released firmware updates itself by asking
`api.github.com/repos/mcyork/7segclock/releases/latest` for `tag_name` and then
fetching `releases/latest/download/firmware.bin`. That code is already burned
into devices, so every future release must keep all of this true:

- this repo stays public under the same name;
- the release is a normal release — not a draft, not a pre-release — and is
  marked *Latest*;
- the tag is `vX.Y.Z` (a leading `v` is stripped; three numeric parts);
- an asset named exactly `firmware.bin` is attached, an **app image** (not the
  factory image), at most `0x1E0000` bytes, built for the ESP32-C3;
- the partition table stays `min_spiffs` — OTA cannot change it.

`bun Release.ts --check` verifies the tree, `--build` builds and refreshes the
installer image in `docs/`, `--publish` tags the built commit and creates the
release. Nothing else should ever create a release by hand.

Tag `v1.0.0` predates this contract and points at the initial commit, not at the
commit its binary was built from; it is left alone rather than rewritten.

## Build

```
pio run                                        # build
pio run -t upload                              # flash over USB
pio run -t upload --upload-port mini7seg.local # OTA
```

Every dependency in `platformio.ini` is pinned to an exact version (platform,
FastLED, and the segment library [mcyork/mini7seg](https://github.com/mcyork/mini7seg)
at a specific commit) so a tagged commit rebuilds the same firmware. Do not loosen
the pins; bump them deliberately.

## Licence

MIT
