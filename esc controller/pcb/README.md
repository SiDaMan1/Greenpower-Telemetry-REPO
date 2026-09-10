# ESC Controller PCB — rev D

A carrier board for the `throttle_controller` sketch. The ESP32 DevKit V1 (30-pin)
plugs into two female header strips. The SSD1306 OLED solders **flush to the board**
through four plated holes. Everything else leaves the board on **1 × 2 mm solder
pads** for flying leads.

**One component on the whole board: R1, a 4k7 pull-down on the ESC signal.**

Open `esc-controller.kicad_pro` in KiCad 7 or newer.

```
Board        73 × 60 mm, 2 layer, 1.6 mm
Copper       1 oz, GND pour both sides
Tracks       0.25 mm signal / 0.5 mm power
Vias         0.8 mm pad / 0.4 mm drill
Min clear.   0.3 mm  (well inside every cheap fab's limits)
DRC          0 violations, 0 unconnected pads, 0 footprint errors
```

### What changed from rev C

Area-optimised: **98 × 62 mm → 73 × 60 mm**, a 28% smaller board (6076 → 4380 mm²)
and 25 mm narrower. Nothing electrical changed — same nets, same pads, same parts.

- Three tight columns instead of loose ones: wire pads at the left edge, the
  DevKit socket, then the OLED with the UART pads tucked into the dead space
  underneath it
- **R1 now stands vertically** (2.54 mm pitch, one lead bent down) so it fits the
  3 mm lane between the pad labels and the socket. Same 4k7 resistor, just mounted
  on end — bend one lead back alongside the body before you fit it
- 5V IN moved to the left column, next to the VIN pin it feeds
- Smaller silkscreen text and tighter pad-group spacing throughout
- The two modules now take up 51% of the board area; the rest is wire pads,
  mounting holes and edge margin

### What changed from rev B

- ECO and SPORT are no longer two separate 2-pad groups. They're now **one 3-pad
  MODE SW group** — `ECO / COM / SPORT` — for a single 3-position switch
- Connectors renumbered again: TRIGGER is **J5**, UART OUT is **J6**, 5V IN is **J7**

### What changed from rev A

- All pull-ups, filter caps, bulk cap, power LED and its resistor removed
- R1 (4k7) rewired from series to a **pull-down**, ESC signal to ground
- OLED changed to four plated holes for flush mounting
- Board grew 95 → 98 mm wide

---

## The mode switch

`MODE SW` (J4) takes one **SP3T / 3-position** switch. Wire the switch's common
terminal — the centre pin on almost every slide switch — to the middle pad, and the
two outer terminals to `ECO` and `SPORT`. Because GND sits between the two signals,
the switch's three pins map straight across without any wire crossing over another.

| Switch position | ECO pin | SPORT pin | Mode |
|---|---|---|---|
| One end | pulled low | high | ECO |
| **Centre** | high | high | **NORMAL** |
| Other end | high | pulled low | SPORT |

That falls out of the firmware as written: both inputs are active-low, and with
neither pulled down it lands on NORMAL. There's no way for both to be low at once
with a single switch, so the "ECO wins if both are low" tie-break in the sketch
simply never fires — harmless to leave in.

Which end gives you ECO and which gives SPORT is just which way round you solder
the two outer wires. Swap them if it ends up backwards.

---

## What R1 does

GPIO25 goes straight to the `ESC OUT / SIG` pad. R1 sits across that signal to
ground.

The ESP32's pins are high-impedance during reset and for the first moments of
boot — nothing is driving them. Without a pull-down, the ESC input floats during
that window and can read as anything, which is how a car twitches or lurches when
you power it up. R1 holds the line at 0 V until the firmware takes over. It also
holds the ESC off if the signal wire ever comes loose.

At 3.3 V it draws 0.7 mA when GPIO25 is high — nothing next to what the pin can
source, and negligible against the 31 kHz switching.

**One thing to watch:** if your ESC's signal input has its own internal pull-up,
R1 forms a voltage divider with it and the idle level won't reach 0 V. If the
throttle won't fully close, or the ESC sees a standing signal with the ESP32
unplugged, that's the cause — swap R1 for 10 kΩ or higher. The pads take any
value; it's a one-part change, no board revision needed.

---

## ⚠️ Two things to check before you order

**1. The DevKit row pitch.** The socket assumes **25.4 mm (1.0") between the two
pin rows** — standard for a 30-pin DOIT DevKit V1, but some clones are 22.86 mm
(0.9"). Measure yours, centre-of-pin to centre-of-pin across the rows. If it's
different, change `ROW_SPACING` in `generator/design.py` and re-run
`generator/build.py`. Nothing else has to change.

**2. Where the OLED's pin row sits.** The four holes are on 2.54 mm pitch — that
part is universal. I've placed them **centred on the top edge** of the module and
drawn a 27.3 × 27.8 mm body outline on the silkscreen as a guide. If your module's
header is offset rather than centred, the module will just sit a few mm off from
the printed outline — the board area underneath is completely clear, so nothing
collides. Cosmetic only.

Pin order is **GND, VCC, SCL, SDA** left to right, viewed from above with the
screen facing up. Every hole is labelled on the silkscreen — check them against
your module before you solder, because getting GND and VCC swapped will kill the
display.

---

## Wiring — what goes on which pad

All solder pads are on the top layer, labelled on the silkscreen.

### Left edge

| Group | Pad | Net | Connect to | ESP32 |
|---|---|---|---|---|
| **POT** (J2) | 3V3 | +3V3 | pot terminal 1 | — |
| | WIPER | POT_W | pot wiper | GPIO34 |
| | GND | GND | pot terminal 3 | — |
| **ESC OUT** (J3) | SIG | ESC_SIG | ESC signal wire | GPIO25 direct, R1 pulls it down |
| | GND | GND | ESC ground | — |
| **MODE SW** (J4) | ECO | ECO | switch outer terminal | GPIO27 |
| | COM | GND | switch **common** (centre pin) | — |
| | SPORT | SPORT | switch other outer terminal | GPIO14 |
| **TRIGGER** (J5) | SW | TRIG | trigger switch, one side | GPIO13 |
| | GND | GND | trigger switch, other side | — |
| **5V IN** (J7) | 5V | +5V | BEC / buck output — **optional** | VIN |
| | GND | GND | BEC ground | — |

The trigger and both mode inputs are active-low and rely on the **ESP32's internal
pull-ups** — make sure the sketch declares these pins as `INPUT_PULLUP`, not plain
`INPUT`, or they'll float.

### Right side and OLED

| Group | Pad | Net | Connect to | ESP32 |
|---|---|---|---|---|
| **OLED** (J1) | GND, VCC, SCL, SDA | — | module solders flush, pins down | GPIO22 = SCL, GPIO21 = SDA |
| **UART OUT** (J6, under the OLED) | TX | UART_TX | display_receiver RX | GPIO17 |
| | RX | UART_RX | display_receiver TX | GPIO16 |
| | GND | GND | display_receiver GND | — |

**J7 is optional.** You're on USB power, so leave it unpopulated. It's there so the
board can run standalone later without a redesign. If you do use it, **don't leave
USB plugged in at the same time** — the DevKit V1 has no isolation between the USB
5V rail and VIN.

---

## Bill of materials

| Ref | Value | Package | Qty | Notes |
|---|---|---|---|---|
| U1 | 1×15 female header, 2.54 mm | THT | 2 | Cut from a 40-pin strip. The DevKit sits in these. |
| R1 | 4.7 kΩ | axial, **mounted vertically**, 2.54 mm | 1 | Pull-down, ESC signal to GND |
| H1–H4 | M3 | 3.2 mm hole | 4 | Mounting |

That's the entire BOM. J1 is four plated holes — the OLED module's own pins go
through them, no header needed. J2–J7 are bare solder pads. The mode switch,
trigger, pot and ESC are all off-board on flying leads.

---

## Assembly order

1. **R1** first. It mounts **standing up**: bend one lead 180° back along the body
   so both leads come out of the same end, 2.54 mm apart. Either way round; a
   resistor isn't polarised.
2. **The two 1×15 female strips for U1.** Plug the DevKit into them while you
   solder so the rows end up parallel, but keep it unpowered.
3. **The OLED.** Its pin header goes through J1 from the top with the module lying
   flat on the board; solder underneath and trim the pins flush. Check the four
   silkscreen labels against the module first.
4. **Flying leads** to the solder pads. Tin the pad, tin the wire, lay the wire flat
   on the pad and reflow. Add a dab of hot glue after testing — an unsupported
   soldered wire will fatigue and snap in a race car.

The OLED sits directly on the board, so the copper under it is bare board and
solder mask only — no components, nothing to short against.

---

## Manufacturing

`gerbers.zip` uploads to JLCPCB / PCBWay / OSHPark as-is (Gerber X2 + Excellon
drill, mm). Default settings: 2 layer, 1.6 mm, HASL, any colour. At 73 × 60 mm
it's comfortably inside the 100 × 100 mm price break — roughly $2–5 for five
boards plus shipping.

### If you need it smaller still

Two things set the size, and both are choices rather than physics:

- **The four M3 mounting holes** cost roughly 7 mm of board at each corner. Two
  holes instead of four, or M2.5 instead of M3, would let the board come in
  another 4–5 mm each way.
- **The layout is three columns.** Stacking the OLED *below* the DevKit instead
  of beside it gives a long, narrow board (about 45 × 85 mm) — slightly less area,
  but a much less square shape. Better if it mounts along a dash rail, worse if
  it goes in a box.

Say which you'd prefer and it's a placement change in `design.py`, not a redesign.

---

## Regenerating

The project is generated from source, so changes stay consistent:

```
cd generator
python3 build.py      # places, routes, checks clearances, writes the .kicad_pcb
python3 gen_sch.py    # writes the .kicad_sch and .kicad_pro
```

`design.py` holds everything worth changing — board size, `ROW_SPACING`, placement,
the pad groups and the netlist. `router.py` is a two-layer maze router with
clearance checking; `build.py` places, routes, then runs its own geometric
clearance and connectivity checks before writing the board. Needs `numpy`.

---

## Verification done on this design

- KiCad's own DRC: **0 violations, 0 unconnected pads, 0 footprint errors**
  (see `drc_report.txt`)
- Independent geometric clearance check: 0 pairs closer than 0.20 mm
- Independent connectivity check: every net's pads joined by copper
- Schematic netlist exported and diffed against the PCB netlist — **all 12 nets
  match exactly**, same pads on the same nets, same footprints
- `ECO` confirmed as J4 pad 1 → U1 pin 10 (GPIO27), `SPORT` as J4 pad 3 → U1
  pin 11 (GPIO14), and J4 pad 2 on GND
- `ESC_SIG` confirmed to carry exactly U1 pin 8 (GPIO25), J3 pad 1 and R1 pad 1,
  with R1 pad 2 landing in the ground pour
- Every GPIO cross-checked against `SYSTEM_INFO.md`, and every pin confirmed to
  exist on the 30-pin DevKit V1
- Thermal relief checked: every ground through-hole pad gets at least two spokes
  into the pour, so no pad hangs off the plane by a single thin bridge

The two things software can't check for you are the 25.4 mm row pitch and your
OLED's pin order. Both are one measurement each.
