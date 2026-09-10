#!/usr/bin/env python3
"""Build the ESC-controller KiCad PCB: place, route, verify, emit."""
import os, sys, uuid, math, json
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from design import (BOARD_W, BOARD_H, NETS, NETID, COMPONENTS, SILK,
                    ESP32_PINS, ROW_SPACING, PIN_PITCH, REFPOS, REF_ON_FAB,
                    STITCH_VIAS)
import router as R

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
PROJ = "esc-controller"
os.makedirs(OUT, exist_ok=True)

def U():
    return str(uuid.uuid4())

def f(v):
    return ("%.4f" % v).rstrip("0").rstrip(".") or "0"

# ---------------------------------------------------------------- flatten pads
PADS = []   # dict: ref, num, kind, shape, x, y, w, h, drill, net, name, comp
for comp in COMPONENTS:
    ax, ay = comp["at"]
    for (num, kind, shape, (lx, ly), (w, h), drill, net, name) in comp["pads"]:
        PADS.append(dict(ref=comp["ref"], num=num, kind=kind, shape=shape,
                         x=ax + lx, y=ay + ly, w=w, h=h, drill=drill,
                         net=net, name=name, comp=comp))

# ---------------------------------------------------------------- router setup
rt = R.Router(BOARD_W, BOARD_H, edge_margin=1.2)
for p in PADS:
    if p["kind"] == "npth":
        rt.add_keepout(p["x"], p["y"], 3.0)
    else:
        rt.add_pad(p["kind"], p["shape"], p["x"], p["y"], p["w"], p["h"], NETID[p["net"]])

# (component bodies sit on soldermask, so routing under them is fine)

# Reserve a small ring around every through-hole GND pad. GND is carried by the
# pour rather than by traces, so if signals are allowed to crowd right up to
# these pads the zone can only reach them with one thermal spoke, which KiCad
# (rightly) calls a starved connection. Nothing routes GND, so a hard keepout
# here costs nothing.
for p in PADS:
    if p["net"] == "GND" and p["kind"] == "tht":
        rt.add_keepout(p["x"], p["y"], 1.6)

# ---------------------------------------------------------------- route
POWER = {"+3V3", "+5V"}
order = ["POT_W", "ESC_SIG", "ECO", "SPORT", "TRIG",
         "SCL", "SDA", "UART_TX", "UART_RX", "+5V", "+3V3"]

fails = []
for net in order:
    nid = NETID[net]
    nodes = []
    for p in PADS:
        if p["net"] != net:
            continue
        cells = rt.pad_cells(p["kind"], p["shape"], p["x"], p["y"], p["w"], p["h"])
        cells = [c for c in cells if 0 <= c[1] < rt.nx and 0 <= c[2] < rt.ny]
        if not cells:
            print("  !! no grid cells for pad %s.%s" % (p["ref"], p["num"]))
        nodes.append(dict(cells=cells, center=(p["x"], p["y"]),
                          tag="%s.%s" % (p["ref"], p["num"])))
    w = R.POWER_W if net in POWER else R.TRACE_W
    ok, bad = rt.route_net(nid, nodes, width=w)
    if not ok:
        for b in bad:
            fails.append((net, b["tag"]))
    print("route %-9s nodes=%d %s" % (net, len(nodes), "OK" if ok else "FAILED:%s" % [b["tag"] for b in bad]))

# ---------------------------------------------------------------- GND stitching
# Every SMD GND pad gets a via beside it so it reaches the B.Cu pour as well as
# the F.Cu one. Each candidate position is checked against the occupancy grid
# first -- dropping a via blindly next to a pad is how you short a neighbour.
GND = NETID["GND"]
placed = dropped = 0
for p in PADS:
    if p["net"] != "GND" or p["kind"] != "smd":
        continue
    dirx = 1.0 if p["x"] < BOARD_W / 2 else -1.0
    for (dx, dy) in [(2.4, 0), (2.4, 1.6), (2.4, -1.6), (3.2, 0),
                     (0, 1.8), (0, -1.8), (1.8, 1.8), (1.8, -1.8),
                     (4.0, 0), (3.2, 2.2), (3.2, -2.2), (4.0, 1.8), (4.0, -1.8),
                     (-2.4, 0), (-2.4, 1.6), (-2.4, -1.6)]:
        vx, vy = p["x"] + dirx * dx, p["y"] + dy
        if not rt.region_free([0, 1], vx, vy, R.VIA_SIZE, R.VIA_SIZE, R.CLEAR, GND):
            continue
        if not rt.place_stub(p["x"], p["y"], vx, vy, 0, GND, 0.4):
            continue
        if rt.place_via(vx, vy, GND):
            placed += 1
            break
    else:
        dropped += 1
        print("  note: no room for a stitching via at %s.%s (pour still connects it on F.Cu)"
              % (p["ref"], p["num"]))

for (vx, vy) in STITCH_VIAS:
    if rt.place_via(vx, vy, GND):
        placed += 1
    else:
        print("  note: stitching via at (%.1f, %.1f) does not fit, skipped" % (vx, vy))
print("GND stitching vias placed: %d (dropped: %d)" % (placed, dropped))

SEGMENTS = rt.segments
VIAS = rt.vias

print("\nsegments=%d vias=%d" % (len(SEGMENTS), len(VIAS)))
if fails:
    print("UNROUTED CONNECTIONS:", fails)

# ---------------------------------------------------------------- DRC check
def rect_of_pad(p):
    return (p["x"] - p["w"] / 2, p["y"] - p["h"] / 2,
            p["x"] + p["w"] / 2, p["y"] + p["h"] / 2)

items = []   # (net_key, layers, rect, label)
for p in PADS:
    if p["kind"] == "npth":
        continue
    key = p["net"] if p["net"] else "NC:%s.%s" % (p["ref"], p["num"])
    layers = (0, 1) if p["kind"] == "tht" else (0,)
    items.append((key, layers, rect_of_pad(p), "pad %s.%s" % (p["ref"], p["num"])))
for (x1, y1, x2, y2, l, n, w) in SEGMENTS:
    items.append((NETS[n], (l,),
                  (min(x1, x2) - w / 2, min(y1, y2) - w / 2,
                   max(x1, x2) + w / 2, max(y1, y2) + w / 2),
                  "seg L%d" % l))
for (x, y, n) in VIAS:
    items.append((NETS[n], (0, 1),
                  (x - R.VIA_SIZE / 2, y - R.VIA_SIZE / 2,
                   x + R.VIA_SIZE / 2, y + R.VIA_SIZE / 2), "via"))

def gap(a, b):
    dx = max(a[0] - b[2], b[0] - a[2], 0.0)
    dy = max(a[1] - b[3], b[1] - a[3], 0.0)
    return math.hypot(dx, dy)

MIN_CLEAR = 0.20
viol = []
for i in range(len(items)):
    ki, li, ri, ni = items[i]
    for j in range(i + 1, len(items)):
        kj, lj, rj, nj = items[j]
        if ki == kj:
            continue
        if not set(li) & set(lj):
            continue
        g = gap(ri, rj)
        if g < MIN_CLEAR:
            viol.append((round(g, 3), ki, ni, kj, nj))
viol.sort()
print("clearance violations (<%.2fmm): %d" % (MIN_CLEAR, len(viol)))
for v in viol[:40]:
    print("   ", v)

# NPTH keepout check
npth_bad = []
for p in PADS:
    if p["kind"] != "npth":
        continue
    hr = (p["x"] - 3.0, p["y"] - 3.0, p["x"] + 3.0, p["y"] + 3.0)
    for (k, l, r, n) in items:
        if gap(hr, r) <= 0:
            npth_bad.append((p["ref"], k, n))
print("mounting-hole keepout hits: %d" % len(npth_bad), npth_bad[:10])

# connectivity check: every net's pads must be joined by segments/vias
def connectivity_report():
    import collections
    bad = []
    for net in NETS[1:]:
        if net == "GND":
            continue    # handled by the copper pour
        nid = NETID[net]
        nodes = [p for p in PADS if p["net"] == net]
        if len(nodes) < 2:
            continue
        # union-find over pads + segment endpoints on this net
        parent = {}
        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]; a = parent[a]
            return a
        def union(a, b):
            ra, rb = find(a), find(b)
            if ra != rb: parent[ra] = rb
        objs = []
        for p in nodes:
            objs.append(("pad", rect_of_pad(p), (0, 1) if p["kind"] == "tht" else (0,),
                         "%s.%s" % (p["ref"], p["num"])))
        for (x1, y1, x2, y2, l, n, w) in SEGMENTS:
            if n != nid: continue
            objs.append(("seg", (min(x1,x2)-w/2, min(y1,y2)-w/2, max(x1,x2)+w/2, max(y1,y2)+w/2),
                         (l,), "seg"))
        for (x, y, n) in VIAS:
            if n != nid: continue
            objs.append(("via", (x-R.VIA_SIZE/2, y-R.VIA_SIZE/2, x+R.VIA_SIZE/2, y+R.VIA_SIZE/2),
                         (0,1), "via"))
        for i in range(len(objs)): parent[i] = i
        for i in range(len(objs)):
            for j in range(i+1, len(objs)):
                if not set(objs[i][2]) & set(objs[j][2]): continue
                if gap(objs[i][1], objs[j][1]) <= 1e-6:
                    union(i, j)
        roots = set(find(i) for i in range(len(objs)) if objs[i][0] == "pad")
        if len(roots) > 1:
            groups = collections.defaultdict(list)
            for i in range(len(objs)):
                if objs[i][0] == "pad": groups[find(i)].append(objs[i][3])
            bad.append((net, list(groups.values())))
    return bad

conn_bad = connectivity_report()
print("nets not fully connected (excluding GND pour): %d" % len(conn_bad))
for b in conn_bad:
    print("   ", b)

# ---------------------------------------------------------------- emit .kicad_pcb
LAYERS = """  (layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive")
    (33 "F.Adhes" user "F.Adhesive")
    (34 "B.Paste" user)
    (35 "F.Paste" user)
    (36 "B.SilkS" user "B.Silkscreen")
    (37 "F.SilkS" user "F.Silkscreen")
    (38 "B.Mask" user)
    (39 "F.Mask" user)
    (40 "Dwgs.User" user "User.Drawings")
    (41 "Cmts.User" user "User.Comments")
    (42 "Eco1.User" user "User.Eco1")
    (43 "Eco2.User" user "User.Eco2")
    (44 "Edge.Cuts" user)
    (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard")
    (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user)
    (49 "F.Fab" user)
  )
"""

SETUP = """  (setup
    (pad_to_mask_clearance 0.0508)
    (allow_soldermask_bridges_in_footprints no)
    (pcbplotparams
      (layerselection 0x00010fc_ffffffff)
      (plot_on_all_layers_selection 0x0000000_00000000)
      (disableapertmacros false)
      (usegerberextensions false)
      (usegerberattributes true)
      (usegerberadvancedattributes true)
      (creategerberjobfile true)
      (dashed_line_dash_ratio 12.000000)
      (dashed_line_gap_ratio 3.000000)
      (svgprecision 4)
      (plotframeref false)
      (viasonmask false)
      (mode 1)
      (useauxorigin false)
      (hpglpennumber 1)
      (hpglpenspeed 20)
      (hpglpendiameter 15.000000)
      (pdf_front_fp_property_popups true)
      (pdf_back_fp_property_popups true)
      (dxfpolygonmode true)
      (dxfimperialunits true)
      (dxfusepcbnewfont true)
      (psnegative false)
      (psa4output false)
      (plotreference true)
      (plotvalue true)
      (plotinvisibletext false)
      (sketchpadsonfab false)
      (subtractmaskfromsilk false)
      (outputformat 1)
      (mirror false)
      (drillshape 1)
      (scaleselection 1)
      (outputdirectory "gerbers/")
    )
  )
"""

def eff(size=1.0, thick=0.15, justify=None, mirror=False):
    j = " (justify %s)" % justify if justify else ""
    return "(effects (font (size %s %s) (thickness %s))%s)" % (f(size), f(size), f(thick), j)

def pad_sexp(p, indent="    "):
    net = ""
    if p["net"]:
        net = " (net %d \"%s\")" % (NETID[p["net"]], p["net"])
    if p["kind"] == "npth":
        return ('%s(pad "" np_thru_hole circle (at %s %s) (size %s %s) (drill %s) '
                '(layers "F&B.Cu" "*.Mask") (tstamp %s))\n'
                % (indent, f(p["lx"]), f(p["ly"]), f(p["w"]), f(p["h"]), f(p["drill"]), U()))
    if p["kind"] == "tht":
        return ('%s(pad "%s" thru_hole %s (at %s %s) (size %s %s) (drill %s) '
                '(layers "*.Cu" "*.Mask")%s (tstamp %s))\n'
                % (indent, p["num"], p["shape"], f(p["lx"]), f(p["ly"]),
                   f(p["w"]), f(p["h"]), f(p["drill"]), net, U()))
    return ('%s(pad "%s" smd rect (at %s %s) (size %s %s) '
            '(layers "F.Cu" "F.Mask")%s (tstamp %s))\n'
            % (indent, p["num"], f(p["lx"]), f(p["ly"]), f(p["w"]), f(p["h"]), net, U()))

def fp_line(x1, y1, x2, y2, layer, w=0.12, indent="    "):
    return ('%s(fp_line (start %s %s) (end %s %s) (stroke (width %s) (type solid)) '
            '(layer "%s") (tstamp %s))\n' % (indent, f(x1), f(y1), f(x2), f(y2), f(w), layer, U()))

def fp_rect_lines(x1, y1, x2, y2, layer, w=0.12):
    s = ""
    s += fp_line(x1, y1, x2, y1, layer, w)
    s += fp_line(x2, y1, x2, y2, layer, w)
    s += fp_line(x2, y2, x1, y2, layer, w)
    s += fp_line(x1, y2, x1, y1, layer, w)
    return s

def footprint_sexp(comp):
    ax, ay = comp["at"]
    ref, val, fpn = comp["ref"], comp["value"], comp["fp"]
    # local pad list
    locals_ = []
    for (num, kind, shape, (lx, ly), (w, h), drill, net, name) in comp["pads"]:
        locals_.append(dict(num=num, kind=kind, shape=shape, lx=lx, ly=ly,
                            w=w, h=h, drill=drill, net=net, name=name))
    xs = [p["lx"] for p in locals_] or [0]
    ys = [p["ly"] for p in locals_] or [0]
    attr = "through_hole" if any(p["kind"] == "tht" for p in locals_) else "smd"
    if all(p["kind"] == "npth" for p in locals_):
        attr = "through_hole exclude_from_pos_files exclude_from_bom"

    s = '  (footprint "esc:%s" (layer "F.Cu")\n' % fpn
    s += "    (tstamp %s)\n" % U()
    s += "    (at %s %s)\n" % (f(ax), f(ay))
    if comp.get("desc"):
        s += '    (descr "%s")\n' % comp["desc"].replace('"', "'")
    s += "    (attr %s)\n" % attr

    # ---- reference designator
    rdx, rdy, rj = REFPOS.get(ref, ((min(xs) + max(xs)) / 2, min(ys) - 2.4, None))
    rlayer = "F.Fab" if ref in REF_ON_FAB else "F.SilkS"
    s += ('    (fp_text reference "%s" (at %s %s) (layer "%s") (tstamp %s) %s)\n'
          % (ref, f(rdx), f(rdy), rlayer, U(), eff(0.95, 0.15, rj)))
    s += ('    (fp_text value "%s" (at %s %s) (layer "F.Fab") hide (tstamp %s) %s)\n'
          % (val, f((min(xs) + max(xs)) / 2), f(max(ys) + 2.4), U(), eff(0.9)))

    # ---- graphics
    if comp["ref"] == "U1":
        bx0, by0, bx1, by1 = comp["body"]
        s += fp_rect_lines(bx0, by0, bx1, by1, "F.SilkS", 0.15)
        s += fp_rect_lines(bx0 - 0.25, by0 - 0.25, bx1 + 0.25, by1 + 0.25, "F.CrtYd", 0.05)
        s += fp_line(-2.9, -1.6, -2.9, 1.6, "F.SilkS", 0.2)       # pin-1 marker
        s += fp_line(-2.9, 0, -1.7, 0, "F.SilkS", 0.2)
        for p in locals_:
            side = "right" if p["lx"] < 1 else "left"
            tx = p["lx"] - 1.4 if p["lx"] < 1 else p["lx"] + 1.4
            s += ('    (fp_text user "%s" (at %s %s) (layer "F.Fab") (tstamp %s) %s)\n'
                  % (p["name"], f(tx), f(p["ly"]), U(), eff(0.7, 0.1, side)))
    elif comp["ref"].startswith("H"):
        pass
    elif attr == "smd":
        # wire-pad group: no outline box (keeps the silkscreen readable),
        # just the group name above and a name beside every pad
        x0, x1 = min(xs) - 1.3, max(xs) + 1.3
        y0, y1 = min(ys) - 1.1, max(ys) + 1.1
        s += fp_rect_lines(x0 - 0.2, y0 - 0.2, x1 + 0.2, y1 + 0.2, "F.CrtYd", 0.05)
        left_side = (ax < BOARD_W / 2)
        # right-hand groups sit 4mm from the board edge, so their name is
        # right-justified and grows inward instead of off the board
        if left_side:
            lx_, lj_ = (min(xs) + max(xs)) / 2, None
        else:
            lx_, lj_ = max(xs) + 1.0, "right"
        s += ('    (fp_text user "%s" (at %s %s) (layer "F.SilkS") (tstamp %s) %s)\n'
              % (comp.get("label", ""), f(lx_), f(min(ys) - 2.2),
                 U(), eff(0.9, 0.15, lj_)))
        for p in locals_:
            tx = p["lx"] + 1.9 if left_side else p["lx"] - 1.9
            j = "left" if left_side else "right"
            s += ('    (fp_text user "%s" (at %s %s) (layer "F.SilkS") (tstamp %s) %s)\n'
                  % (p["name"], f(tx), f(p["ly"]), U(), eff(0.8, 0.13, j)))
            # small tick pointing at the pad so labels are unambiguous
        s += fp_line(min(xs) - 1.2, min(ys) - 1.25, max(xs) + 1.2, min(ys) - 1.25,
                     "F.SilkS", 0.12)
    else:
        if comp.get("silk_box"):
            x0, y0, x1, y1 = comp["silk_box"]
        else:
            x0, x1 = min(xs) - 1.3, max(xs) + 1.3
            y0, y1 = min(ys) - 1.3, max(ys) + 1.3
        s += fp_rect_lines(x0, y0, x1, y1, "F.SilkS", 0.12)
        s += fp_rect_lines(x0 - 0.2, y0 - 0.2, x1 + 0.2, y1 + 0.2, "F.CrtYd", 0.05)
        if comp["ref"] == "J1":
            for i, p in enumerate(locals_):
                ly = -2.6 if i % 2 == 0 else -4.6      # stagger, 2.54mm is tight
                s += ('    (fp_text user "%s" (at %s %s) (layer "F.SilkS") (tstamp %s) %s)\n'
                      % (p["name"], f(p["lx"]), f(ly), U(), eff(0.8, 0.13)))
            s += ('    (fp_text user "OLED" (at %s %s) (layer "F.SilkS") (tstamp %s) %s)\n'
                  % (f(3.81), f(-7.2), U(), eff(1.2, 0.18)))
            # mark pin 1 outside the module body
            s += fp_line(-1.9, -1.4, -1.9, 1.4, "F.SilkS", 0.2)

    for p in locals_:
        s += pad_sexp(p)
    s += "  )\n"
    return s

def gr_text(t, x, y, size, layer, justify=None, thick=None):
    th = thick if thick else max(0.12, size * 0.15)
    return ('  (gr_text "%s" (at %s %s) (layer "%s") (tstamp %s) %s)\n'
            % (t, f(x), f(y), layer, U(), eff(size, th, justify)))

def build_pcb():
    s = '(kicad_pcb (version 20221018) (generator pcbnew)\n\n'
    s += "  (general\n    (thickness 1.6)\n  )\n\n"
    s += '  (paper "A4")\n\n'
    s += LAYERS + "\n" + SETUP + "\n"
    for i, n in enumerate(NETS):
        s += '  (net %d "%s")\n' % (i, n)
    s += "\n"
    for comp in COMPONENTS:
        s += footprint_sexp(comp) + "\n"
    # board outline
    pts = [(0, 0), (BOARD_W, 0), (BOARD_W, BOARD_H), (0, BOARD_H), (0, 0)]
    for a, b in zip(pts, pts[1:]):
        s += ('  (gr_line (start %s %s) (end %s %s) (stroke (width 0.1) (type solid)) '
              '(layer "Edge.Cuts") (tstamp %s))\n' % (f(a[0]), f(a[1]), f(b[0]), f(b[1]), U()))
    s += "\n"
    for (t, x, y, size, rot, layer) in SILK:
        s += gr_text(t, x, y, size, layer)
    s += "\n"
    for (x1, y1, x2, y2, l, n, w) in SEGMENTS:
        s += ('  (segment (start %s %s) (end %s %s) (width %s) (layer "%s") (net %d) (tstamp %s))\n'
              % (f(x1), f(y1), f(x2), f(y2), f(w), "F.Cu" if l == 0 else "B.Cu", n, U()))
    for (x, y, n) in VIAS:
        s += ('  (via (at %s %s) (size %s) (drill %s) (layers "F.Cu" "B.Cu") (net %d) (tstamp %s))\n'
              % (f(x), f(y), f(R.VIA_SIZE), f(R.VIA_DRILL), n, U()))
    s += "\n"
    # ground pour, both layers
    m = 0.4
    poly = [(m, m), (BOARD_W - m, m), (BOARD_W - m, BOARD_H - m), (m, BOARD_H - m)]
    s += ('  (zone (net %d) (net_name "GND") (layers "F.Cu" "B.Cu") (tstamp %s) (hatch edge 0.508)\n'
          '    (connect_pads (clearance 0.35))\n'
          '    (min_thickness 0.2) (filled_areas_thickness no)\n'
          '    (fill yes (thermal_gap 0.3) (thermal_bridge_width 0.5))\n'
          '    (polygon\n      (pts\n        %s\n      )\n    )\n  )\n'
          % (NETID["GND"], U(),
             " ".join("(xy %s %s)" % (f(x), f(y)) for x, y in poly)))
    s += ")\n"
    return s

pcb = build_pcb()
with open(os.path.join(OUT, PROJ + ".kicad_pcb"), "w") as fh:
    fh.write(pcb)
print("\nwrote %s.kicad_pcb  (%d bytes)" % (PROJ, len(pcb)))

# dump geometry for the preview renderer
with open(os.path.join(OUT, "_geom.json"), "w") as fh:
    json.dump(dict(board=[BOARD_W, BOARD_H],
                   pads=[{k: v for k, v in p.items() if k != "comp"} for p in PADS],
                   segments=SEGMENTS, vias=VIAS, silk=SILK,
                   comps=[dict(ref=c["ref"], at=c["at"], body=c.get("body"),
                               pads=[(p[0], p[3][0], p[3][1]) for p in c["pads"]])
                          for c in COMPONENTS]), fh)
