# Light Dance Firmware

Controller-side scripts for running the Light Dance show: broadcasts time
pulses to the Pico 2W dancers over UDP, plays the music track in sync,
and previews each dancer's lights in a Qt window.

## Files

- `control3.py` — main controller + UI entry point.
- `fetch_lightdata.py` — downloads the show data and caches it to `lightdata.npz`.
- `ui.py` — Qt window / dancer-figure widgets.
- `2026_show.mp3` — bundled music track.
- `main.cpp` — Pico 2W firmware for costume units (LED body parts).
- `main_props.cpp` — Pico 2W firmware for prop units.
- `platformio.ini` — PlatformIO build config for the Pico 2W firmware.

There are two big things to do: **(A) upload firmware to each Pico 2W**,
and **(B) run the controller on your computer**.

---

# Part A — Upload firmware to a Pico 2W

Each costume / prop runs the firmware in `main.cpp` (costumes) or
`main_props.cpp` (props). Flash one Pico 2W at a time with a unique
`PLAYER_NUM`.

### 1. Set up the PlatformIO project

Copy `platformio.ini` into a fresh PlatformIO project, then drop the
firmware source in as the project's `src/main.cpp`:

- Costume unit → copy `main.cpp` to `src/main.cpp`.
- Prop unit    → copy `main_props.cpp` to `src/main.cpp`.

### 2. Set the player number

Open `src/main.cpp` and edit the `PLAYER_NUM` define near the top:

```c
#define PLAYER_NUM     0     // unique per costume / prop
```

This identifies the unit on the network, picks its static IP, and
selects the per-player config (e.g. the `SECTIONS` table in
`main_props.cpp`). Every unit in the show must have a different value.

### 3. Build and upload

Build and upload with PlatformIO (`pio run -t upload`, or the upload
button in your IDE) over USB.

### 4. Boot in download mode to fetch the latest lightdata

After flashing, set the DIP switches so the Pico boots in **download
mode** and power-cycle it:

- `DEBUG_PIN` (GPIO 18) — **high** (normal boot, not debug)
- `SWITCH_PIN` (GPIO 17) — **high** (download & halt)
- `WIFI_PIN`  (GPIO 20) — pick the WiFi profile with internet access
  (high = `EE219B` / DHCP, low = `Lightdance` / static IP)

The unit connects to WiFi, downloads its frames from the server,
writes them to flash via LittleFS, and halts. Watch the OLED for
progress.

### 5. Switch to load mode and reboot

Flip the DIP switches for **load mode**, then power-cycle:

- `SWITCH_PIN` (GPIO 17) — **low** (load from flash & run)
- `WIFI_PIN`  (GPIO 20) — **switch to the broadcast WiFi profile** —
  normally **low** (`Lightdance`, static IP `192.168.1.{100+PLAYER_NUM}`
  for costumes / `152+PLAYER_NUM` for props). Don't forget this — if
  you leave it on the internet profile from step 4, the unit will join
  the wrong network and `control3.py` won't see it.

The Pico now loads the saved animation from flash, joins the broadcast
LAN, and waits for time pulses from `control3.py`. From here on it no
longer needs internet — only the broadcast network.

---

# Part B — Run the controller

Follow these steps in order. Steps 1 and 2 need internet; step 3 onward
runs on the internal broadcast LAN.

### 1. Fetch light data (needs internet)

`lightdata.npz` is the per-player frame timeline used to preview the
show in the UI. Download it once while you still have internet —
`control3.py` will then load from this cache, no internet required at
show time.

```
python fetch_lightdata.py                     # default user "eesa3", LATEST
python fetch_lightdata.py <user>              # custom user, LATEST
python fetch_lightdata.py <user> <time>       # custom user + timestamp
```

The `.npz` is written next to the script and picked up automatically.
Re-run this whenever the show data changes.

### 2. Download the music and update the path (needs internet)

Grab the music track for the show and save it locally, then open
`control3.py` and point `MUSIC_FILE` at the absolute path where you
saved it:

```python
MUSIC_FILE = r"C:\School_2025\LightDance\Firmware\firmware\2026_show.mp3"
```

`MUSIC_OFFSET` (just below) shifts the music vs. the broadcast clock in
seconds — positive = music ahead, negative = music behind.

> **Reminder:** every machine / audio setup has a different latency, so
> you'll likely need to **tune `MUSIC_OFFSET` per computer**. Do a dry
> run with the dancers and nudge it a few tenths of a second at a time
> until the music lines up with the lights. Re-check it whenever you
> change machines, audio outputs (e.g. plugging in different
> speakers), or the music file itself.

### 3. Switch to the internal broadcast network

Connect your computer to the internal LAN that the Pico 2W dancers are
on. The controller auto-detects your IP and derives the broadcast
address (`x.y.z.255`) from it, so you must be on the same subnet as the
dancers — otherwise the heartbeats and time pulses will not reach them.

### 4. Run the controller

```
python control3.py
```

This opens the dancer-grid window, discovers Pico 2W dancers on the LAN
via UDP heartbeats, and on **Start** plays the music and broadcasts the
time clock so every dancer stays in sync. Type a start offset in
seconds in the input box if you want to begin partway through the show.

---

# Appendix — Editing the light SECTIONS table (props)

Prop firmware (`main_props.cpp`) maps the 8 packed color slots in each
frame (`acc0..acc7`) onto **physical LED strips**. Each prop unit has
its own layout, so when you build a new prop or rewire an old one,
you'll need to add or update its `SECTIONS` table.

### What the table means

```c
struct Section { uint8_t strip, start, count, slot; };
```

Each row says: paint `count` LEDs on `strip` starting at index `start`,
using the color from `slot` (which is column `slot + 1` of the frame —
column 0 is the timestamp).

- `strip`: which physical chain — `0 = PROP1 (GP2)`, `1 = PROP2 (GP3)`,
  `2 = PROP3 (GP4)`.
- `start`: first LED index on that strip (0-based).
- `count`: how many LEDs in this section.
- `slot`: which `accN` slot drives them (`0..7`).

`STRIP_LENS[3]` is the LED count per strip. It must cover every
section's `start + count` on that strip. Strips with length `0` are
skipped at `FastLED.addLeds()` time.

### How it's wired up

`main_props.cpp` selects the table at compile time with
`#if PLAYER_NUM == ...` blocks. To add a new player, add a new branch
before `#else` and define both `SECTIONS[]` and `STRIP_LENS[3]`:

```c
#elif PLAYER_NUM == 6
// Dancer 7: 3 LEDs on strip 0 driven by acc0..2.
const Section SECTIONS[] = {
    {0, 0, 1, 0}, {0, 1, 1, 1}, {0, 2, 1, 2},
};
const uint8_t STRIP_LENS[3] = {3, 0, 0};
```

Existing examples in `main_props.cpp`:

- `PLAYER_NUM == 2` — 2 props on strip 0, slots `acc0..1`.
- `PLAYER_NUM == 3` — blade (`acc0..3`, strip 0), handle (`acc4..5`,
  strip 1), hilt (`acc6..7`, strip 2).
- `PLAYER_NUM == 4 || 5` — blade (`acc0..1`), handle (`acc2..3`), hilt
  (`acc4`).

### Constraints to watch

- **No slot collisions on overlapping LEDs.** Two sections can share a
  slot (mirroring the same color across LEDs is fine), but two
  sections that paint the same physical LED with different slots will
  fight each other.
- **`STRIP_LENS[i]` must be ≥ the highest `start + count` on strip i**,
  otherwise FastLED writes past the strip's buffer.
- **Don't exceed `MAX_LEDS_PER_STRIP`** (8 by default). Bump it in
  `main_props.cpp` if a new prop needs more LEDs per chain.
- After editing the table, rebuild and re-flash that unit (Part A
  steps 3–5). The on-flash lightdata does not need to be re-downloaded
  unless the show data itself changed.
