"""
Design database for the ESC Controller PCB -- rev D (area-optimised).

Board: 73 x 60 mm, 2 layer.
  - ESP32 DevKit V1 (30-pin DOIT) sits in 2x15 female headers
  - SSD1306 0.96" OLED solders flush to the board through 4 plated holes
  - everything else leaves the board on 1 x 2 mm solder pads
  - one component only: R1, 4k7 pull-down on the ESC signal (holds the
    ESC input low while the ESP32 is in reset / booting)

Coordinates in mm, KiCad convention (origin top-left, Y down).
"""

BOARD_W = 73.0
BOARD_H = 60.0

NETS = [
    "",            # 0
    "GND",         # 1
    "+3V3",        # 2
    "+5V",         # 3
    "POT_W",       # 4
    "ESC_SIG",     # 5
    "TRIG",        # 6
    "ECO",         # 7
    "SPORT",       # 8
    "SDA",         # 9
    "SCL",         # 10
    "UART_TX",     # 11
    "UART_RX",     # 12
]
NETID = {n: i for i, n in enumerate(NETS)}

# ---------------------------------------------------------------- footprints
# Each component: ref, value, fp (footprint name), at (x,y), pads[]
# pad: (number, kind, shape, (lx,ly), (w,h), drill, net, name)
#   kind: 'tht' | 'smd' | 'npth'

WIREPAD = (2.0, 1.0)      # 1mm x 2mm solder pad for wires (long axis outward)

ESP32_PINS = [
    # (pin, name, net)
    (1,  "EN",     ""),
    (2,  "GPIO36", ""),
    (3,  "GPIO39", ""),
    (4,  "GPIO34", "POT_W"),
    (5,  "GPIO35", ""),
    (6,  "GPIO32", ""),
    (7,  "GPIO33", ""),
    (8,  "GPIO25", "ESC_SIG"),
    (9,  "GPIO26", ""),
    (10, "GPIO27", "ECO"),
    (11, "GPIO14", "SPORT"),
    (12, "GPIO12", ""),
    (13, "GPIO13", "TRIG"),
    (14, "GND",    "GND"),
    (15, "VIN",    "+5V"),
    (16, "GPIO23", ""),
    (17, "GPIO22", "SCL"),
    (18, "TX0",    ""),
    (19, "RX0",    ""),
    (20, "GPIO21", "SDA"),
    (21, "GPIO19", ""),
    (22, "GPIO18", ""),
    (23, "GPIO5",  ""),
    (24, "GPIO17", "UART_TX"),
    (25, "GPIO16", "UART_RX"),
    (26, "GPIO4",  ""),
    (27, "GPIO2",  ""),
    (28, "GPIO15", ""),
    (29, "GND",    "GND"),
    (30, "3V3",    "+3V3"),
]

ROW_SPACING = 25.4        # <-- measure your board! 1.0" is standard DOIT V1 30-pin
PIN_PITCH = 2.54


def esp32_socket(at):
    pads = []
    for pin, name, net in ESP32_PINS:
        if pin <= 15:
            lx, ly = 0.0, (pin - 1) * PIN_PITCH
        else:
            lx, ly = ROW_SPACING, (pin - 16) * PIN_PITCH
        shape = "rect" if pin == 1 else "circle"
        pads.append((str(pin), "tht", shape, (lx, ly), (1.8, 1.8), 1.0, net, name))
    return dict(ref="U1", value="ESP32-DevKitV1-30p",
                fp="ESP32_DEVKITV1_30P_SOCKET", at=at, pads=pads,
                body=(-1.45, -7.97, 26.85, 43.53),   # module outline, local
                desc="2x15 female header, 2.54mm pitch, 25.4mm rows")


def wirepads(ref, at, entries, desc):
    """entries: list of (net, label). Pads stack downward at 3mm pitch."""
    pads = []
    for i, (net, label) in enumerate(entries):
        pads.append((str(i + 1), "smd", "rect", (0.0, i * 3.0), WIREPAD, None, net, label))
    return dict(ref=ref, value="WirePads", fp="SOLDERPAD_1x2MM_%dP" % len(entries),
                at=at, pads=pads, desc=desc)


def header1x(ref, value, at, entries, desc, horiz=False, fp=None):
    pads = []
    for i, (net, label) in enumerate(entries):
        shape = "rect" if i == 0 else "circle"
        loc = (i * 2.54, 0.0) if horiz else (0.0, i * 2.54)
        pads.append((str(i + 1), "tht", shape, loc, (1.7, 1.7), 1.0, net, label))
    return dict(ref=ref, value=value,
                fp=fp or ("PinHeader_1x%02d_P2.54mm_Vertical" % len(entries)),
                at=at, pads=pads, desc=desc, horiz=horiz)


def axial(ref, value, at, pitch, n1, n2, fp, pad=1.6, drill=0.8, vert=False):
    p2 = (0.0, pitch) if vert else (pitch, 0.0)
    pads = [("1", "tht", "rect", (0.0, 0.0), (pad, pad), drill, n1, ""),
            ("2", "tht", "circle", p2, (pad, pad), drill, n2, "")]
    return dict(ref=ref, value=value, fp=fp, at=at, pads=pads, pitch=pitch)


def mount(ref, at):
    return dict(ref=ref, value="MountingHole_M3", fp="MountingHole_3.2mm_M3",
                at=at, pads=[("", "npth", "circle", (0.0, 0.0), (3.2, 3.2), 3.2, "", "")])


def _lbl(comp, text):
    comp["label"] = text
    return comp


# ---------------------------------------------------------------- placement
# Three columns, packed tight:
#   A  x 2.5-8    wire pads + labels, all the off-board wiring except UART
#   B  x 13-41.3  the ESP32 DevKit, USB pointing at the bottom edge
#   C  x 43.3-71  the OLED on top, UART pads in the space underneath it
# R1 stands vertically in the 3mm lane between columns A and B.
COMPONENTS = []

# --- ESP32 socket -----------------------------------------------------------
COMPONENTS.append(esp32_socket((14.45, 11.0)))     # body spans x 13.0-41.3

# --- COLUMN A: wire pads, ordered to match the DevKit's left pin row so no
#     two signal traces have to cross on the way in
COMPONENTS.append(_lbl(wirepads("J2", (3.5, 9.5),
                               [("+3V3", "3V3"), ("POT_W", "WIPER"), ("GND", "GND")],
                               "Throttle potentiometer"), "POT"))
COMPONENTS.append(_lbl(wirepads("J3", (3.5, 20.5),
                               [("ESC_SIG", "SIG"), ("GND", "GND")],
                               "ESC signal output - direct from GPIO25, R1 pulls it down"),
                       "ESC OUT"))
COMPONENTS.append(_lbl(wirepads("J4", (3.5, 28.5),
                               [("ECO", "ECO"), ("GND", "COM"), ("SPORT", "SPORT")],
                               "3-position mode switch: ECO / centre=NORMAL / SPORT"),
                       "MODE SW"))
COMPONENTS.append(_lbl(wirepads("J5", (3.5, 39.5),
                               [("TRIG", "SW"), ("GND", "GND")],
                               "Trigger / throttle button"), "TRIGGER"))
COMPONENTS.append(_lbl(wirepads("J7", (3.5, 47.5),
                               [("+5V", "5V"), ("GND", "GND")],
                               "OPTIONAL 5V input (BEC) - leave open when USB powered"),
                       "5V IN"))

# --- the only component, stood on end to save board -------------------------
# R1 sits across the ESC signal to ground. GPIO25 is high-impedance during
# reset and boot, so without this the ESC input floats and the car can twitch
# at power-on. Pad 2 lands in the ground pour.
COMPONENTS.append(axial("R1", "4k7", (10.5, 20.5), 2.54, "ESC_SIG", "GND",
                        "Resistor_THT_Axial_P2.54mm_Vertical", vert=True))

# --- COLUMN C: OLED on four plated holes, module solders flush --------------
_j1 = header1x("J1", "SSD1306_0.96in", (53.39, 10.0),
               [("GND", "GND"), ("+3V3", "VCC"), ("SCL", "SCL"), ("SDA", "SDA")],
               "SSD1306 OLED - solder module flush, pins through board",
               horiz=True, fp="OLED_SSD1306_096_FLUSH_1x4")
_j1["silk_box"] = (-10.09, -2.0, 17.71, 25.3)    # 27.3 x 27.8 mm module body
COMPONENTS.append(_j1)

# UART pads live in the dead space under the OLED, at the right edge
COMPONENTS.append(_lbl(wirepads("J6", (68.0, 40.0),
                               [("UART_TX", "TX"), ("UART_RX", "RX"), ("GND", "GND")],
                               "UART to display_receiver"), "UART OUT"))

# --- mounting holes, tucked into the four corners ---------------------------
for _i, (_x, _y) in enumerate([(3.5, 3.5), (69.5, 3.5), (3.5, 56.5), (69.5, 56.5)]):
    COMPONENTS.append(mount("H%d" % (_i + 1), (_x, _y)))


# ---------------------------------------------------------------- silkscreen
SILK = [
    # (text, x, y, size, rot, layer)
    ("USB THIS END", 27.15, 56.6, 0.9, 0, "F.SilkS"),
    ("ESC CONTROLLER rev D", 27.15, 58.8, 1.0, 0, "F.SilkS"),
    ("MODE SW CENTRE = NORMAL", 55.0, 50.0, 0.85, 0, "F.SilkS"),
    ("R1 = PULL-DOWN TO GND", 55.0, 52.6, 0.85, 0, "F.SilkS"),
    ("GreenPower F24", 55.0, 55.2, 0.85, 0, "F.SilkS"),
]

# reference-designator placement, local to each footprint origin:
#   (dx, dy, justify)  --  justify None = centred
REFPOS = {
    "J1": (3.81, 28.6, None),
    "R1": (0.0, -2.9, None),
}
# refs that carry no assembly meaning go on F.Fab so the silkscreen stays clean
REF_ON_FAB = {"U1", "J2", "J3", "J4", "J5", "J6", "J7",
              "H1", "H2", "H3", "H4"}

# extra ground-stitching vias, placed in open copper (any that do not fit are
# skipped automatically by build.py)
STITCH_VIAS = [(10.5, 6.5), (10.5, 52.0), (27.0, 6.5), (27.0, 51.0),
               (46.0, 40.0), (46.0, 50.0), (63.0, 5.0), (57.0, 55.5)]


# Give every wire-pad group its own footprint name: the silkscreen labels
# differ per group, so sharing one library footprint would mismatch.
import re as _re
for _c in COMPONENTS:
    if _c.get("label") and _c["fp"].startswith("SOLDERPAD"):
        _slug = _re.sub(r"[^A-Z0-9]+", "_", _c["label"].upper()).strip("_")
        _c["fp"] = "SOLDERPAD_%s_%dP" % (_slug, len(_c["pads"]))
