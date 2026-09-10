#!/usr/bin/env python3
"""Generate the ESC-controller schematic (.kicad_sch) + project file.

Connectivity is expressed with global labels rather than long drawn wires:
every pin gets a short stub and a named label, which is how KiCad joins nets
anyway and keeps a one-page sheet readable.
"""
import os, sys, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from design import COMPONENTS, ESP32_PINS

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
PROJ = "esc-controller"
ROOT_UUID = str(uuid.uuid4())

def U(): return str(uuid.uuid4())
def f(v): return ("%.4f" % v).rstrip("0").rstrip(".") or "0"
def E(sz=1.27, just=None, hide=False):
    j = " (justify %s)" % just if just else ""
    return "(effects (font (size %s %s))%s%s)" % (f(sz), f(sz), j, " hide" if hide else "")

# --------------------------------------------------------------- symbol defs
def sym_box(name, left, right, w=None, ref="U", extra_h=2.54):
    """left/right: list of (number, pinname, etype). Returns lib symbol text."""
    n = max(len(left), len(right))
    h = (n - 1) * 2.54 + 2 * extra_h
    if w is None:
        longest = max([len(p[1]) for p in left + right] + [4])
        w = max(15.24, longest * 1.6 + 10.16)
    hw, hh = w / 2, h / 2
    s = '    (symbol "esc:%s" (pin_names (offset 0.762)) (in_bom yes) (on_board yes)\n' % name
    s += '      (property "Reference" "%s" (at 0 %s 0) %s)\n' % (ref, f(hh + 2.54), E())
    s += '      (property "Value" "%s" (at 0 %s 0) %s)\n' % (name, f(-hh - 2.54), E())
    s += '      (property "Footprint" "" (at 0 0 0) %s)\n' % E(hide=True)
    s += '      (property "Datasheet" "" (at 0 0 0) %s)\n' % E(hide=True)
    s += '      (symbol "%s_0_1"\n' % name
    s += ('        (rectangle (start %s %s) (end %s %s) '
          '(stroke (width 0.254) (type default)) (fill (type background)))\n'
          % (f(-hw), f(hh), f(hw), f(-hh)))
    s += "      )\n"
    s += '      (symbol "%s_1_1"\n' % name
    pins = []
    for i, (num, pname, et) in enumerate(left):
        y = hh - extra_h - i * 2.54
        s += ('        (pin %s line (at %s %s 0) (length 5.08) '
              '(name "%s" %s) (number "%s" %s))\n'
              % (et, f(-hw - 5.08), f(y), pname, E(), num, E()))
        pins.append((num, -hw - 5.08, y))
    for i, (num, pname, et) in enumerate(right):
        y = hh - extra_h - i * 2.54
        s += ('        (pin %s line (at %s %s 180) (length 5.08) '
              '(name "%s" %s) (number "%s" %s))\n'
              % (et, f(hw + 5.08), f(y), pname, E(), num, E()))
        pins.append((num, hw + 5.08, y))
    s += "      )\n    )\n"
    return s, dict(pins={p[0]: (p[1], p[2]) for p in pins})


def sym_2pin(name, kind, ref):
    """Vertical 2-pin part: pin 1 on top, pin 2 on bottom."""
    s = '    (symbol "esc:%s" (pin_numbers hide) (pin_names (offset 0) hide) (in_bom yes) (on_board yes)\n' % name
    s += '      (property "Reference" "%s" (at 2.54 1.27 0) %s)\n' % (ref, E(1.27, "left"))
    s += '      (property "Value" "%s" (at 2.54 -1.27 0) %s)\n' % (name, E(1.27, "left"))
    s += '      (property "Footprint" "" (at 0 0 0) %s)\n' % E(hide=True)
    s += '      (property "Datasheet" "" (at 0 0 0) %s)\n' % E(hide=True)
    s += '      (symbol "%s_0_1"\n' % name
    if kind == "R":
        s += ('        (rectangle (start -1.016 2.54) (end 1.016 -2.54) '
              '(stroke (width 0.254) (type default)) (fill (type none)))\n')
        plen = 1.27
    elif kind == "C":
        s += ('        (polyline (pts (xy -2.032 0.762) (xy 2.032 0.762)) '
              '(stroke (width 0.508) (type default)) (fill (type none)))\n')
        s += ('        (polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762)) '
              '(stroke (width 0.508) (type default)) (fill (type none)))\n')
        plen = 3.048
    elif kind == "CP":
        s += ('        (rectangle (start -2.032 0.508) (end 2.032 1.016) '
              '(stroke (width 0.0) (type default)) (fill (type outline)))\n')
        s += ('        (polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762)) '
              '(stroke (width 0.508) (type default)) (fill (type none)))\n')
        s += ('        (polyline (pts (xy -2.54 2.54) (xy -1.524 2.54)) '
              '(stroke (width 0.254) (type default)) (fill (type none)))\n')
        s += ('        (polyline (pts (xy -2.032 3.048) (xy -2.032 2.032)) '
              '(stroke (width 0.254) (type default)) (fill (type none)))\n')
        plen = 3.048
    else:  # LED
        s += ('        (polyline (pts (xy -1.27 1.27) (xy -1.27 -1.27) (xy 1.27 0) (xy -1.27 1.27)) '
              '(stroke (width 0.254) (type default)) (fill (type none)))\n')
        s += ('        (polyline (pts (xy 1.27 1.27) (xy 1.27 -1.27)) '
              '(stroke (width 0.254) (type default)) (fill (type none)))\n')
        s += ('        (polyline (pts (xy 1.778 2.032) (xy 3.048 3.302)) '
              '(stroke (width 0.152) (type default)) (fill (type none)))\n')
        s += ('        (polyline (pts (xy 0.508 2.286) (xy 1.778 3.556)) '
              '(stroke (width 0.152) (type default)) (fill (type none)))\n')
        plen = 2.54
    s += "      )\n"
    s += '      (symbol "%s_1_1"\n' % name
    s += ('        (pin passive line (at 0 %s 270) (length %s) (name "~" %s) (number "1" %s))\n'
          % (f(2.54 + plen), f(plen), E(), E()))
    s += ('        (pin passive line (at 0 %s 90) (length %s) (name "~" %s) (number "2" %s))\n'
          % (f(-2.54 - plen), f(plen), E(), E()))
    s += "      )\n    )\n"
    return s, dict(pins={"1": (0.0, 2.54 + plen), "2": (0.0, -2.54 - plen)})


# --------------------------------------------------------------- build sheet
LIB = ""
GEOM = {}

# ESP32 module symbol
left = [(str(p), n, "power_in" if n in ("VIN",) else
         ("passive" if n == "GND" else "bidirectional"))
        for p, n, _ in ESP32_PINS if p <= 15]
right = [(str(p), n, "power_out" if n == "3V3" else
          ("passive" if n == "GND" else "bidirectional"))
         for p, n, _ in ESP32_PINS if p > 15]
t, g = sym_box("ESP32_DEVKIT_V1_30P", left, right, w=33.02, ref="U")
LIB += t; GEOM["ESP32_DEVKIT_V1_30P"] = g

# connector symbols, one per distinct pin count / label set
CONN_SYMS = {}
for c in COMPONENTS:
    if not c["fp"].startswith(("SOLDERPAD", "PinHeader", "OLED_")):
        continue
    name = "CONN_" + c["ref"]
    pins = [(p[0], p[7] or ("P%s" % p[0]), "passive") for p in c["pads"]]
    t, g = sym_box(name, pins, [], w=15.24, ref="J")
    LIB += t; GEOM[name] = g
    CONN_SYMS[c["ref"]] = name

# zero-pin mounting-hole symbol, so "Update PCB from Schematic" doesn't
# report H1..H4 as footprints with no matching symbol
LIB += ('    (symbol "esc:MountingHole" (in_bom yes) (on_board yes)\n'
        '      (property "Reference" "H" (at 0 3.81 0) (effects (font (size 1.27 1.27))))\n'
        '      (property "Value" "MountingHole_M3" (at 0 -3.81 0) (effects (font (size 1.27 1.27))))\n'
        '      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
        '      (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
        '      (symbol "MountingHole_0_1"\n'
        '        (circle (center 0 0) (radius 1.27) (stroke (width 0.254) (type default)) (fill (type none)))\n'
        '      )\n    )\n')
GEOM["MountingHole"] = dict(pins={})

t, g = sym_2pin("R", "R", "R")
LIB += t; GEOM["R"] = g

# ---- placement on an A3 sheet ---------------------------------------------
SHEET = []      # symbol instances
WIRES = []
LABELS = []
NOTES = []

def place(libname, ref, value, fp, x, y, netmap, ref_off=(0, 0)):
    """netmap: {pinnumber: netname}. Emits the symbol, stubs and labels."""
    g = GEOM[libname]
    s = ('  (symbol (lib_id "esc:%s") (at %s %s 0) (unit 1) (in_bom yes) (on_board yes) '
         '(dnp no)\n    (uuid %s)\n' % (libname, f(x), f(y), U()))
    s += '    (property "Reference" "%s" (at %s %s 0) %s)\n' % (
        ref, f(x + ref_off[0]), f(y + ref_off[1]), E(1.27, "left"))
    s += '    (property "Value" "%s" (at %s %s 0) %s)\n' % (
        value, f(x + ref_off[0]), f(y + ref_off[1] + 2.0), E(1.27, "left"))
    s += '    (property "Footprint" "esc:%s" (at %s %s 0) %s)\n' % (fp, f(x), f(y), E(1.27, hide=True))
    s += '    (property "Datasheet" "" (at %s %s 0) %s)\n' % (f(x), f(y), E(1.27, hide=True))
    for num in g["pins"]:
        s += '    (pin "%s" (uuid %s))\n' % (num, U())
    s += ('    (instances (project "%s" (path "/%s" (reference "%s") (unit 1))))\n  )\n'
          % (PROJ, ROOT_UUID, ref))
    SHEET.append(s)
    for num, (px, py) in g["pins"].items():
        net = netmap.get(num)
        if not net:
            continue
        sx, sy = x + px, y - py            # symbol space is Y-up, sheet is Y-down
        if abs(px) > abs(py):
            dx = -5.08 if px < 0 else 5.08
            ex, ey, ang = sx + dx, sy, (180 if dx < 0 else 0)
        else:
            dy = -5.08 if py > 0 else 5.08
            ex, ey, ang = sx, sy + dy, (90 if dy < 0 else 270)
        WIRES.append((sx, sy, ex, ey))
        LABELS.append((net, ex, ey, ang))

# ESP32
esp_net = {str(p): n for p, _, n in ESP32_PINS if n}
place("ESP32_DEVKIT_V1_30P", "U1", "ESP32-DevKitV1-30p", "ESP32_DEVKITV1_30P_SOCKET",
      160.0, 110.0, esp_net, ref_off=(-16.0, -30.0))

# connectors: left column then right column
left_refs = ["J2", "J3", "J4", "J5"]
right_refs = ["J1", "J6", "J7"]
cmap = {c["ref"]: c for c in COMPONENTS}
ypos = 40.0
for r in left_refs:
    c = cmap[r]
    nm = CONN_SYMS[r]
    n = len(c["pads"])
    place(nm, r, c.get("label", c["value"]), c["fp"], 55.0, ypos,
          {p[0]: p[6] for p in c["pads"]}, ref_off=(-8.0, -((n - 1) * 1.27 + 8.0)))
    ypos += (n - 1) * 2.54 + 24.0
ypos = 40.0
for r in right_refs:
    c = cmap[r]
    nm = CONN_SYMS[r]
    n = len(c["pads"])
    place(nm, r, c.get("label", c["value"]), c["fp"], 262.0, ypos,
          {p[0]: p[6] for p in c["pads"]}, ref_off=(-8.0, -((n - 1) * 1.27 + 8.0)))
    ypos += (n - 1) * 2.54 + 24.0

# the only component on the board
for i, c in enumerate([c for c in COMPONENTS if "pitch" in c]):
    place("R", c["ref"], c["value"], c["fp"], 335.0, 45.0 + i * 30.48,
          {"1": c["pads"][0][6], "2": c["pads"][1][6]}, ref_off=(3.0, -2.0))

for _i, _h in enumerate(["H1", "H2", "H3", "H4"]):
    place("MountingHole", _h, "M3", "MountingHole_3.2mm_M3",
          40.0 + _i * 20.0, 265.0, {}, ref_off=(-3.0, 6.0))

NOTES = [
    ("ESC CONTROLLER  -  ESP32 DevKit V1 (30 pin)  -  rev C", 15.0, 20.0, 3.0),
    ("Wire pads J2..J7 are 1 x 2 mm SMD pads for flying leads.", 15.0, 190.0, 1.8),
    ("J1 is four plated holes - the SSD1306 OLED solders flush to the board, pins down.", 15.0, 196.0, 1.8),
    ("R1 (4k7) is the only component: a pull-down holding the ESC signal low while the ESP32 boots.", 15.0, 202.0, 1.8),
    ("Mode switches rely on the ESP32 internal pull-ups (INPUT_PULLUP in firmware).", 15.0, 208.0, 1.8),
    ("J4 is one 3-position switch: common to GND, centre position = NORMAL mode.", 15.0, 214.0, 1.8),
    ("J7 (5V IN) is optional and must not be fed from a BEC while USB is plugged in.", 15.0, 220.0, 1.8),
    ("+3V3 is generated by the DevKit's own regulator (U1 pin 30).", 15.0, 226.0, 1.8),
]


def build():
    s = "(kicad_sch (version 20230121) (generator eeschema)\n\n"
    s += "  (uuid %s)\n\n" % ROOT_UUID
    s += '  (paper "A3")\n\n'
    s += ('  (title_block\n    (title "ESC Controller")\n    (date "")\n'
          '    (rev "A")\n    (company "GreenPower F24")\n  )\n\n')
    s += "  (lib_symbols\n" + LIB + "  )\n\n"
    for (x1, y1, x2, y2) in WIRES:
        s += ('  (wire (pts (xy %s %s) (xy %s %s)) '
              '(stroke (width 0) (type default)) (uuid %s))\n'
              % (f(x1), f(y1), f(x2), f(y2), U()))
    s += "\n"
    for (net, x, y, ang) in LABELS:
        just = "left" if ang == 0 else ("right" if ang == 180 else "left")
        s += ('  (global_label "%s" (shape bidirectional) (at %s %s %s) (fields_autoplaced)\n'
              '    (effects (font (size 1.27 1.27)) (justify %s))\n    (uuid %s)\n'
              '    (property "Intersheetrefs" "${INTERSHEET_REFS}" (at %s %s 0) %s)\n  )\n'
              % (net, f(x), f(y), ang, just, U(), f(x), f(y), E(1.27, hide=True)))
    s += "\n" + "".join(SHEET) + "\n"
    for (t, x, y, sz) in NOTES:
        s += ('  (text "%s" (at %s %s 0) (effects (font (size %s %s)) (justify left bottom)) (uuid %s))\n'
              % (t, f(x), f(y), f(sz), f(sz), U()))
    s += '\n  (sheet_instances\n    (path "/" (page "1"))\n  )\n)\n'
    return s


sch = build()
open(os.path.join(OUT, PROJ + ".kicad_sch"), "w").write(sch)
print("wrote %s.kicad_sch (%d bytes, %d symbols, %d labels)"
      % (PROJ, len(sch), len(SHEET), len(LABELS)))

# ------------------------------------------------------------- project file
pro = {
    "board": {"design_settings": {"rules": {
        "min_clearance": 0.2, "min_track_width": 0.2,
        "min_via_diameter": 0.6, "min_through_hole_diameter": 0.3,
        "min_hole_clearance": 0.25, "min_silk_clearance": 0.0}},
        "layer_presets": [], "viewports": []},
    "boards": [],
    "cvpcb": {"equivalence_files": []},
    "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
    "meta": {"filename": PROJ + ".kicad_pro", "version": 1},
    "net_settings": {"classes": [{
        "bus_width": 12, "clearance": 0.25, "diff_pair_gap": 0.25,
        "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "line_style": 0,
        "microvia_diameter": 0.3, "microvia_drill": 0.1, "name": "Default",
        "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
        "track_width": 0.25, "via_diameter": 0.8, "via_drill": 0.4, "wire_width": 6}],
        "meta": {"version": 3}, "net_colors": None},
    "pcbnew": {"last_paths": {"gencad": "", "idf": "", "netlist": "",
                              "specctra_dsn": "", "step": "", "vrml": ""},
               "page_layout_descr_file": ""},
    "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
    "sheets": [[ROOT_UUID, ""]],
    "text_variables": {},
}
import json
open(os.path.join(OUT, PROJ + ".kicad_pro"), "w").write(json.dumps(pro, indent=2))
print("wrote %s.kicad_pro" % PROJ)
