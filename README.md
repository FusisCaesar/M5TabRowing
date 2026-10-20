# M5Tab Rowing

A SpeedCoach-style rowing monitor for the M5Stack Tab5 (ESP32-P4). Stroke rate
comes from the built-in BMI270 accelerometer; split, distance and distance per
stroke come from a GPS unit on the Grove port. Runs standalone, logs to microSD.

![Main screen](shots/main.png)

## Hardware

| Part | Notes |
| --- | --- |
| M5Stack Tab5 | 1280x720 display, BMI270 IMU, microSD slot, battery |
| M5Stack Unit GNSS / GPS v1.1 (ATGM336H-6N, AT6668) | plug into **Port A** (red Grove, G53/G54) |
| microSD card | FAT32 (exFAT is not supported); the `F`/`Y` serial command formats it on the device |
| Waterproof case / phone mount | the Tab5 is not water resistant |

The firmware switches the Grove 5 V output on and turns Port A into a UART. It
scans 115200/9600/38400/57600 baud and both pin orders, so the unit works with
a standard Grove cable. Once found it configures 10 Hz updates (AT6668 /
CASIC commands, with u-blox and MTK fallbacks) and only RMC + GGA sentences.

Mounting: any orientation works. The detector finds the boat's forward axis by
itself (largest horizontal acceleration) and works out which way is forward from
the shape of the stroke, or from GPS once you are moving above 1 m/s. Mount it
rigidly: a wobbly holder adds noise that looks like strokes.

## Building and flashing

PlatformIO with the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform `55.03.312-1` (Arduino core 3.3.12, ESP-IDF 5.5) and the `m5stack-tab5-p4`
board, which targets the pre-rev3 ESP32-P4 silicon in current Tab5 units. The
camera library (`ESP_Video`) needs Arduino core 3.3.11 or later.

```powershell
$env:PYTHONIOENCODING='utf-8'; $env:PYTHONUTF8='1'   # esptool prints non-cp1252 characters
python -m platformio run -e tab5 -t upload --upload-port COM10
```

Environments:

- `tab5` is the real firmware.
- `tab5_sim` generates synthetic rowing (20 -> 32 -> 20 spm over 2 minutes, device
  tilted 35 degrees) and a synthetic GPS track, so the whole pipeline can be tested
  at a desk. The top bar shows a `SIM` badge.

Windows notes:

- `platformio.ini` sets `core_dir = C:/pio` because the ESP32 Arduino libraries
  nest deeper than Windows' 260-character path limit. Delete that line if long
  paths are enabled or you build on Linux/macOS.

## Using it

Six screens; the right-most bottom button cycles through them.

| Screen | Content |
| --- | --- |
| Main | eight metric tiles (split, rate, distance, time, dist/stroke, avg split, heart rate, rec:drive by default); tap any tile to cycle the metric in that slot |
| Map | offline waterway map with your position, heading and track; head-up/north-up toggle and zoom buttons |
| Efficiency | distance per stroke against rate with your best line, efficiency score, ratio/check/bounce gauges and a coaching hint |
| Plot | last 6 s of the forward acceleration with threshold, catch and finish markers, and drive/recovery/check/bounce stats; shows `REC S####` while a session records |
| Settings | three tabs. **STROKE**: threshold K and floor, minima threshold, rec:drive target shift. **DISPLAY**: day/night theme, brightness, flip display, camera mirror image. **SENSORS**: heart rate sensor, GPS+IMU fusion, raw IMU logging |
| Sessions | the recorded sessions on the SD card, to review, rename or delete (see below) |

Bottom buttons: **START** starts a session, **LAP** splits a lap, **hold STOP**
ends the session, **hold RESET** clears it. Stroke rate and split show before
START; distance, time and logging run only during a session.

Tap a metric tile on Main (or the strip above the Map / full-width PIP) to cycle
what it shows: split, rate, distance, time, dist/stroke, avg split, avg rate,
strokes, heart rate, avg HR, rec:drive, speed. The layout is remembered. The
same eight slots are used in every layout, so the four big Main tiles are the
same four as the Map strip.

**PIP** (Main screen only) cycles the picture-in-picture pane through **OFF**,
**CAMERA** and **MAP**. Tap the pane to switch between picture-in-picture (pane
top left, first two tiles big beside it) and full width (first four tiles in a
strip above it).

- **CAMERA** shows the built-in camera, which looks past the rower towards the
  bow, i.e. where the boat is going. It runs only while shown, at about 20 fps,
  and stops on the other screens. The image is mirrored by default, like a
  rear-view mirror; turn that off in Settings.
- **MAP** shows the same offline waterway map as the Map screen.

### Heart rate

Any Bluetooth heart rate sensor works: a chest strap (Polar H10, Garmin HRM,
Wahoo TICKR, ...) or a Garmin watch with **Broadcast Heart Rate** switched on
(controls menu, or "broadcast during activity" in the sensor settings), so you
can keep recording on the watch too. ANT+ is not supported.

In Settings, tap **PAIR** on the heart rate row while the sensor is on and
close by; the strongest one in range is saved. The Tab5 reconnects to it by
itself at every start and whenever it drops out. **FORGET** removes it, after
which you can pair another.

Heart rate is a tile you can put anywhere (it starts in the bottom row; orange
`--` while there is no reading). The top bar shows `HR`, `HR...` (connecting)
or `NO HR`. The lap banner adds the lap's average heart rate.

A chest strap reads much more reliably than a watch while rowing: grip tension
and wrist flexion make optical wrist readings lag or drop out at high rates.

### Map screen

The Map screen draws an offline vector map of the waterways around you, built
from OpenStreetMap data (see below). Water areas and canals are blue, your boat
is the yellow triangle, and the current session's track trails behind it in
orange. The scale bar is bottom-left and the OpenStreetMap credit bottom-right.

- The **HEAD UP** / **BOW UP** / **NORTH UP** control cycles what is at the top
  of the map: the direction the rower looks (stern), the direction of travel
  (bow), or north. A red north arrow shows unless north is already up. During a
  session it is an icon the same size as the zoom buttons (boat bow-up, boat
  head-up, or a compass for north-up); otherwise it is the fourth button under
  the map.
- The **+** / **-** buttons bottom-right zoom in and out. During a session they
  are double size so they are easier to hit.
- Drag the map to look ahead. A locate button appears to the left of **−** while
  the map is not following the boat; tap it to centre back on your position.

The map follows your GPS fix and automatically switches to whichever uploaded
map file covers your position, so you can keep several areas on the card.

![Map screen](shots/map_screen.png)
![Map in picture-in-picture](shots/main_pip_small.png)

### Rhythm and efficiency

**REC:DRIVE** is recovery time divided by drive time, averaged over 5 strokes.
The target follows the stroke rate: about 2.5 at 20 spm, falling to about 1.3
at 34 spm (a long, calm recovery at low rates; at race pace the recovery has to
shrink). Within 15% of the target the tile is green with **GOOD**; below it,
orange **SLOWER** means you are rushing the slide; above it, **QUICKER** means
the recovery drags. Red means more than 30% off. If your coach wants a
different rhythm, shift the whole target up or down in Settings.

The **Efficiency** screen compares each stroke with your own best in this
session at the same rate:

- The chart plots metres per stroke against stroke rate. The blue line is your
  best (3-stroke average, so a single GPS jump does not count), the big dot is
  now. The thin curves are lines of equal split: getting the same split at a
  lower rate means longer strokes, so aim to push your best line up and right.
- **EFFICIENCY** is your current distance per stroke as a percentage of that best
  (it needs about 5 strokes at a rate before it scores).
- The gauges show recovery:drive against its target band, and check and bounce
  against your usual values in this session (lower is better).
- The line above the buttons gives one hint, in order of priority: rhythm,
  then a rough slide (check), then stroke length.

Wind and current change distance per stroke, so the best line is per session:
it clears when you start a new session, and **hold CLEAR** clears it after you
turn into the wind.

![Efficiency screen](shots/efficiency.png)

![Plot screen](shots/plot.png)
![Settings screen](shots/settings.png)

## Session logs

Each session writes `/rowing/S####_*.csv` on the SD card:

| File | One row per | Contents |
| --- | --- | --- |
| `_strokes.csv` | stroke | rate, drive/recovery time, split, speed, distance per stroke, distance, catch depth, check, bounce, position, catch time (`t_s`) |
| `_seconds.csv` | second | UTC, position, speed, split, distance, rate, HDOP, satellites, battery, heart rate (empty without a sensor) |
| `_laps.csv` | lap | lap time, distance, average split and rate, strokes, average and maximum heart rate |
| `_info.csv` | setting | build, start time, and the detector settings in use, so a session can be replayed exactly |
| `_imu.csv` | IMU sample (100 Hz) | raw accelerometer and gyro, filtered and unfiltered surge, vertical, threshold, catch / finish flags, GPS speed (only with raw logging on) |

`t_s` is the same clock in every file, so strokes and IMU samples line up.
Raw logging takes about 13 MB per hour.

Dates and times in the files are UTC from the Tab5's battery-backed clock (RTC).
The clock is set from internet time whenever Wi-Fi is connected and from the GPS
once it has a fix, and keeps time while the Tab5 is off. `_info.csv` records
where the time came from (`time_source`: NTP, GPS or RTC). The top bar and the
Sessions screen show local time (`cfg::TIMEZONE`, Amsterdam by default).

### Sessions screen

Lists every session on the card with its number, start date and time, duration
and size.

- The pencil gives a session a name such as `Bosbaan intervals`. Its files
  become `S0022_Bosbaan_intervals_imu.csv` and so on, and `fetch.py` and
  `analyse.py` understand both forms.
- Tap a session's name to review it: the GPS track on the offline map, coloured
  from red (slow) to green (fast) with a start dot and a finish square, plus
  distance, time, average split, fastest 500 m, average rate, strokes, distance
  per stroke, top speed, average and maximum heart rate, and graphs of split,
  rate and heart rate over the session. Drag the map to pan, +/- to zoom, tap
  the map to fit the whole track again; **BACK** returns to the list. The track
  is drawn without a map when no map on the card covers it.
- A session without a GPS fix still shows what was recorded: time, average
  rate, strokes, laps, average and maximum heart rate, and the rate and heart
  rate graphs.
- The trash can, tapped twice (it turns red after the first tap), removes all
  files of a session.

![Sessions screen](shots/sessions.png)
![Session review](shots/session_review.png)

## Exporting to Strava or Garmin Connect

Fetch the session (or copy it with a card reader), then write GPX and TCX files
next to it:

```powershell
python tools\fetch.py COM10 latest
python tools\export.py 21                    # recordings\S0021.gpx and S0021.tcx
python tools\export.py all --format gpx --out exports
```

- **GPX**: track, time, speed, stroke rate (as cadence) and heart rate, marked as rowing.
  Strava recognises the type; upload it at strava.com/upload/select.
- **TCX**: also distance and one lap per **LAP** press. TCX has no rowing sport,
  so pick rowing after uploading.

Seconds without a GPS fix are left out of the GPX track.

## Recording strokes for detector tuning

1. Check **Raw IMU logging** is ON in Settings (it is by default) and press **START**.
2. The **Plot** screen says `REC S####` while it records.
3. Row a mix: steady state, rate changes, starts, easy paddling, rough water,
   stopping and restarting. Row at least a few minutes so the detector has
   settled, and keep the Tab5 firmly mounted: tilting it mixes gravity into the
   signal.
4. **Hold STOP** when done. Name the session on the **Sessions** screen if you
   like. Then copy the files with a card reader (`/rowing/` on the card), or over
   USB:

   ```powershell
   python tools\fetch.py COM10                 # list the sessions on the card
   python tools\fetch.py COM10 latest          # download the newest into recordings\
   python tools\fetch.py COM10 7 9             # or specific sessions; 'all' for everything
   ```

   About 550 KB/s, so an hour of rowing downloads in under half a minute. Files
   already in `recordings\` are skipped.
5. Analyse a session (needs `pip install numpy matplotlib`):

   ```powershell
   python tools\analyse.py recordings\S0007
   python tools\analyse.py recordings\S0007 --replay --k 2.0 --floor 0.10
   ```

   This writes `recordings\S0007_analysis\` with a `summary.txt`, an overview
   (rate, stroke interval and GPS speed over the session), all strokes overlaid at
   the catch with the average stroke shape per rate band, the detector's view of
   the whole session one minute per page, and a close-up around every suspicious
   stroke (an interval far from its neighbours: probably a missed catch or a
   double count). `--replay` also runs the offline
   detector from `replay.py`, with the session's settings or the ones you give,
   and shows its catches as orange triangles, so you can see whether other
   settings fix a problem.

## Serial tools

Open the port with DTR/RTS held low; otherwise opening it resets the ESP32-P4.
The scripts in `tools/` do this.

```powershell
python tools\serial_log.py COM10 60                       # print status every 5 s and every stroke
python tools\serial_log.py COM10 60 log.txt --send "3:G,40:L,70:X"   # start, lap, stop at those seconds
python tools\send.py COM10 N                              # N next screen, G start, L lap, X stop, R reset
python tools\send.py COM10 C                              # C cycle PIP off/camera/map, V picture-in-picture/full width
python tools\send.py COM10 H                              # H cycle map head-up / bow-up / north-up; +/- zoom the map
python tools\send.py COM10 M                              # M open the Maps screen (download maps)
python tools\send.py COM10 h                              # h pair the strongest heart rate sensor in range
python tools\send.py COM10 'T 640 360'                    # tap at x y; 'P x y' hold, 'Z x0 y0 x1 y1' drag
python tools\send.py COM10 'W MyNetwork|MyPassword'       # join Wi-Fi and remember it; 'W ' reconnects
python tools\serial_log.py COM10 5 --send "1:D"           # list SD session files and /map files with their first lines
python tools\serial_log.py COM10 40 --send "1:F,2:Y"      # erase the SD card: one FAT32 partition, then restart
python tools\fetch.py COM10 latest                        # download the newest session (B path on the wire)
python tools\export.py 21                                 # GPX + TCX of a fetched session
python tools\screenshot.py COM10 shots\main.png           # needs Pillow
```

## Offline maps

Maps are vector `.rmap` files drawn on the device, not raster tiles, so they are
small and fast (a 6 x 6 km city area is about 300 KB). On the water no network is
needed: the map that contains your GPS position is loaded automatically.

### Downloading maps on the Tab5

Tap **MAPS** on the Map screen (between sessions). Wi-Fi switches on while this
screen is open and off again when you leave it.

- **LIBRARY**: well-known rowing waters (Bosbaan, Willem-Alexander Baan, Amstel,
  Henley, Tideway, Rotsee, Bled, ...). Tap the download arrow; for a map already
  on the card, the circular arrow fetches a fresh copy.
- **DRAW AREA**: an OpenStreetMap background centred on your position. Drag to
  pan, +/- to zoom, tap SELECT AREA and drag a rectangle over the water you row
  on (up to 8 x 8 km), then DOWNLOAD AREA.
- **ON CARD**: the maps on the SD card with their size. A map you drew is named
  after its centre (`Area 52.226 5.072`); tap the pencil to give it a proper
  name with the on-screen keyboard. The file name on the card stays the same.
  The trash can, tapped twice, removes a map. Recorded sessions are on the
  **Sessions** screen.

A download takes 30-60 s, mostly waiting for the Overpass map server; the header
shows progress. The Tab5 then converts the data itself (about 2 s) and shows the
new map. Downloads are refused while a session is recording. If the server is
busy, try again a minute later.

![Map library](shots/maps_library.png)
![Drawing an area](shots/maps_draw.png)

**Wi-Fi setup.** The first time, tap **WIFI SETUP** (later: hold the WIFI
button to choose another network). The Tab5 opens its own network
`TabRowing-XXXX` and shows a QR code; scan it with your phone, and a page opens
where you pick your home network (2.4 GHz) and enter its password. The Tab5
remembers it. Over USB you can also send `python tools\send.py COM10 'W MyNetwork|MyPassword'`
(single quotes in PowerShell).

![Wi-Fi setup](shots/maps_setup.png)

Map data is fetched over plain HTTP from overpass-api.de (that server only
accepts TLS 1.3, which the ESP32 TLS stack in this core does not support); the
background map uses HTTPS from tile.openstreetmap.org without certificate
checks. Only public map data is transferred.

### Building maps on a PC

Alternatively build a map with `make_map.py`
(needs `requests`, `osm2geojson` and `shapely`), then upload it to the card.

```powershell
pip install requests osm2geojson shapely
python tools\make_map.py --center 52.3245,4.8360 --radius-km 3 --name "Bosbaan" --out maps\bosbaan.rmap
python tools\send_file.py COM10 maps\bosbaan.rmap /map/bosbaan.rmap
```

`make_map.py` queries the Overpass API for water, bridges, locks, weirs, piers
and rowing clubs, clips and simplifies them, and writes the `.rmap` plus a
`.png` preview so you can check it before uploading. `send_file.py` streams the
file to `/map/` on the SD card in small acked chunks and verifies a CRC; the
firmware picks up the new map without a reboot. Map data is
(c) OpenStreetMap contributors, [ODbL](https://www.openstreetmap.org/copyright).

## On-water validation against an NK SpeedCoach

1. Mount the Tab5 next to the SpeedCoach. Check **Raw IMU logging** is ON in Settings.
2. Start both at roughly the same time. Row a mix: steady state at 18-22 spm,
   a few minutes at 24-28, some starts and bursts at 30+, and some easy paddling.
3. Export the session from NK LiNK Logbook as CSV and copy the SD card files.
4. Compare per stroke. The time offset between the devices is found automatically:

   ```powershell
   python tools\compare_nk.py S0007_strokes.csv nk_export.csv
   ```

   It reports bias, mean absolute error and the 95% range for stroke rate,
   split and distance per stroke, the total distance, and lists strokes whose
   rate differs by 2 spm or more.

5. If strokes are missed or doubled, replay the raw log with other settings and
   copy the best values to the Settings screen:

   ```powershell
   python tools\replay.py S0007_imu.csv --sweep            # grid of K / floor / minima
   python tools\replay.py S0007_imu.csv --k 1.2 --floor 0.08
   python tools\replay.py S0007_imu.csv --plot 60 90       # needs matplotlib
   ```

   `replay.py` is a line-by-line port of `src/stroke_detector.cpp`; keep them in
   sync when changing the detector.

Tuning hints:

- **Missed strokes** (smooth or shallow catches, fixed-seat rowing): lower the
  floor to 0.08 g and K to about 1.2.
- **Double counts** (rough water, a big bounce at the finish): raise K, or turn on
  the minima threshold, which follows the depth of your recent catches.
- Drive and recovery look swapped on the Plot screen during the first strokes
  without GPS: the forward direction is still being learned from the stroke shape.
  It settles within a few strokes, and GPS overrides it after about 10 s of rowing.

## How it works

- **Stroke rate**: gravity is tracked with a slow average and removed. The
  remaining acceleration is band-passed (0.1-3 Hz) and projected onto the
  horizontal axis with the most motion. The catch is the sharp deceleration dip:
  a catch is the midpoint of where the signal crosses an adaptive threshold
  (K x RMS, or 0.6 x recent catch depth) on the way down and up again. Rate is
  the average of the last 3 stroke intervals.
- **Forward direction**: the catch dip is short and deep, the drive is long and
  shallow, so the forward acceleration has negative skew. When GPS is moving, the
  integrated acceleration is correlated with the GPS speed fluctuations, which
  settles the direction even for smooth rowers.
- **Split and distance per stroke**: GPS speed averaged over each stroke cycle;
  distance accumulates GPS positions with a 3 m minimum step so it does not drift
  while stationary. Optionally a complementary filter blends GPS speed with the
  integrated accelerometer for distance per stroke.
- **Check / bounce**: check is how unevenly the boat decelerates during the
  recovery (scatter around a straight line, in mg); bounce is the vertical
  acceleration RMS.
