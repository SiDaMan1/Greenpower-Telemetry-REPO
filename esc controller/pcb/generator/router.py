"""Two-layer Manhattan maze router with clearance checking."""
import heapq
import numpy as np

GRID = 0.25          # mm per cell
TRACE_W = 0.25
POWER_W = 0.5
CLEAR = 0.30         # required copper-to-copper clearance
VIA_SIZE = 0.8
VIA_DRILL = 0.4
VIA_COST = 60        # cells (~15 mm) - discourage layer changes


class Router:
    def __init__(self, w, h, edge_margin=1.2):
        self.w, self.h = w, h
        self.nx = int(round(w / GRID)) + 1
        self.ny = int(round(h / GRID)) + 1
        # occ[layer, x, y] = net id occupying the cell (0 = free)
        self.occ = np.zeros((2, self.nx, self.ny), dtype=np.int16)
        self.segments = []   # (x1,y1,x2,y2,layer,net,width)
        self.vias = []       # (x,y,net)
        # block the board edge
        m = int(round(edge_margin / GRID))
        self.occ[:, :m, :] = -1
        self.occ[:, -m:, :] = -1
        self.occ[:, :, :m] = -1
        self.occ[:, :, -m:] = -1

    # ---------------------------------------------------------------- helpers
    def c2g(self, x, y):
        return int(round(x / GRID)), int(round(y / GRID))

    def g2c(self, gx, gy):
        return gx * GRID, gy * GRID

    def _stamp_rect(self, layers, cx, cy, w, h, inflate, net):
        x0, x1 = cx - w / 2 - inflate, cx + w / 2 + inflate
        y0, y1 = cy - h / 2 - inflate, cy + h / 2 + inflate
        gx0, gy0 = max(0, int(np.floor(x0 / GRID))), max(0, int(np.floor(y0 / GRID)))
        gx1, gy1 = min(self.nx - 1, int(np.ceil(x1 / GRID))), min(self.ny - 1, int(np.ceil(y1 / GRID)))
        for l in layers:
            block = self.occ[l, gx0:gx1 + 1, gy0:gy1 + 1]
            # free -> claim it; claimed by a different net -> nobody may use it
            conflict = (block != 0) & (block != net)
            block[block == 0] = net
            block[conflict] = -1

    def add_pad(self, kind, shape, cx, cy, w, h, net):
        inflate = CLEAR + TRACE_W / 2
        layers = [0, 1] if kind in ("tht", "npth") else [0]
        n = -1 if kind == "npth" else (net if net else -1)
        self._stamp_rect(layers, cx, cy, w, h, inflate, n)

    def add_keepout(self, cx, cy, r, layers=(0, 1)):
        self._stamp_rect(list(layers), cx, cy, 2 * r, 2 * r, 0, -1)

    def pad_cells(self, kind, shape, cx, cy, w, h, margin=0.15):
        """Grid cells lying safely INSIDE the pad copper -- valid route endpoints.

        A track end must overlap the real pad shape for KiCad to call it
        connected, so circular pads are tested as circles, not bounding boxes.
        """
        out = []
        gx0, gy0 = int(np.ceil((cx - w / 2) / GRID)), int(np.ceil((cy - h / 2) / GRID))
        gx1, gy1 = int(np.floor((cx + w / 2) / GRID)), int(np.floor((cy + h / 2) / GRID))
        layers = [0, 1] if kind == "tht" else [0]
        r = min(w, h) / 2 - margin
        for gx in range(gx0, gx1 + 1):
            for gy in range(gy0, gy1 + 1):
                if not (0 <= gx < self.nx and 0 <= gy < self.ny):
                    continue
                px, py = gx * GRID, gy * GRID
                if shape == "circle":
                    if (px - cx) ** 2 + (py - cy) ** 2 > r * r:
                        continue
                else:
                    if abs(px - cx) > w / 2 - margin or abs(py - cy) > h / 2 - margin:
                        continue
                for l in layers:
                    out.append((l, gx, gy))
        return out

    def region_free(self, layers, cx, cy, w, h, inflate, net):
        """True if every cell the shape would occupy is free or already ours."""
        x0, x1 = cx - w / 2 - inflate, cx + w / 2 + inflate
        y0, y1 = cy - h / 2 - inflate, cy + h / 2 + inflate
        gx0, gy0 = int(np.floor(x0 / GRID)), int(np.floor(y0 / GRID))
        gx1, gy1 = int(np.ceil(x1 / GRID)), int(np.ceil(y1 / GRID))
        if gx0 < 0 or gy0 < 0 or gx1 >= self.nx or gy1 >= self.ny:
            return False
        for l in layers:
            block = self.occ[l, gx0:gx1 + 1, gy0:gy1 + 1]
            if np.any((block != 0) & (block != net)):
                return False
        return True

    def place_via(self, x, y, net):
        """Add a via only where it actually fits. Returns True if placed."""
        inflate = CLEAR + VIA_SIZE / 2
        if not self.region_free([0, 1], x, y, VIA_SIZE, VIA_SIZE, CLEAR, net):
            return False
        self.vias.append((x, y, net))
        self._stamp_rect([0, 1], x, y, VIA_SIZE, VIA_SIZE, CLEAR + TRACE_W / 2, net)
        return True

    def place_stub(self, x1, y1, x2, y2, layer, net, width):
        """Add a short trace only where it fits. Returns True if placed."""
        cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
        w = abs(x2 - x1) + width
        h = abs(y2 - y1) + width
        if not self.region_free([layer], cx, cy, w, h, CLEAR, net):
            return False
        self.segments.append((x1, y1, x2, y2, layer, net, width))
        self._stamp_rect([layer], cx, cy, w, h, CLEAR + width / 2, net)
        return True

    # ---------------------------------------------------------------- routing
    def _dijkstra(self, sources, targets, net):
        tgt = set(targets)
        INF = 1 << 30
        dist = np.full((2, self.nx, self.ny), INF, dtype=np.int32)
        prev = {}
        pq = []
        for s in sources:
            l, x, y = s
            if not (0 <= x < self.nx and 0 <= y < self.ny):
                continue
            dist[l, x, y] = 0
            heapq.heappush(pq, (0, l, x, y))
        found = None
        while pq:
            d, l, x, y = heapq.heappop(pq)
            if d > dist[l, x, y]:
                continue
            if (l, x, y) in tgt:
                found = (l, x, y)
                break
            # planar moves
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx_, ny_ = x + dx, y + dy
                if not (0 <= nx_ < self.nx and 0 <= ny_ < self.ny):
                    continue
                o = self.occ[l, nx_, ny_]
                if o != 0 and o != net:
                    continue
                nd = d + 1
                if nd < dist[l, nx_, ny_]:
                    dist[l, nx_, ny_] = nd
                    prev[(l, nx_, ny_)] = (l, x, y)
                    heapq.heappush(pq, (nd, l, nx_, ny_))
            # via
            ol = 1 - l
            o = self.occ[ol, x, y]
            if o == 0 or o == net:
                nd = d + VIA_COST
                if nd < dist[ol, x, y]:
                    dist[ol, x, y] = nd
                    prev[(ol, x, y)] = (l, x, y)
                    heapq.heappush(pq, (nd, ol, x, y))
        if found is None:
            return None
        path = [found]
        while path[-1] in prev:
            path.append(prev[path[-1]])
        path.reverse()
        return path

    def _commit(self, path, net, width):
        """Turn a cell path into segments + vias and stamp occupancy."""
        inflate = CLEAR + width / 2
        # collapse into runs
        runs = []
        i = 0
        while i < len(path) - 1:
            l = path[i][0]
            if path[i + 1][0] != l:                       # via
                runs.append(("via", path[i]))
                i += 1
                continue
            dx = path[i + 1][1] - path[i][1]
            dy = path[i + 1][2] - path[i][2]
            j = i + 1
            while (j < len(path) - 1 and path[j + 1][0] == l
                   and path[j + 1][1] - path[j][1] == dx
                   and path[j + 1][2] - path[j][2] == dy):
                j += 1
            runs.append(("seg", path[i], path[j]))
            i = j
        for r in runs:
            if r[0] == "via":
                _, gx, gy = r[1]
                x, y = self.g2c(gx, gy)
                self.vias.append((x, y, net))
                self._stamp_rect([0, 1], x, y, VIA_SIZE, VIA_SIZE, inflate, net)
            else:
                _, a, b = r
                l = a[0]
                x1, y1 = self.g2c(a[1], a[2])
                x2, y2 = self.g2c(b[1], b[2])
                self.segments.append((x1, y1, x2, y2, l, net, width))
                cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
                w = abs(x2 - x1) + width
                h = abs(y2 - y1) + width
                self._stamp_rect([l], cx, cy, w, h, inflate, net)

    def route_net(self, net, nodes, width=TRACE_W):
        """nodes: list of dicts {cells:[(l,gx,gy)], center:(x,y), kind:str}"""
        if len(nodes) < 2:
            return True, []
        placed = [nodes[0]]
        remaining = list(nodes[1:])
        netcells = set(nodes[0]["cells"])
        failures = []
        while remaining:
            # nearest remaining node to the current net
            def d2(nd):
                px, py = nd["center"]
                return min((px - p["center"][0]) ** 2 + (py - p["center"][1]) ** 2 for p in placed)
            remaining.sort(key=d2)
            nd = remaining.pop(0)
            path = self._dijkstra(list(netcells), nd["cells"], net)
            if path is None:
                failures.append(nd)
                placed.append(nd)
                netcells |= set(nd["cells"])
                continue
            self._commit(path, net, width)
            placed.append(nd)
            netcells |= set(nd["cells"])
            for c in path:
                netcells.add(c)
        return len(failures) == 0, failures

    # ---------------------------------------------------------------- checks
    def check(self):
        """Recompute occupancy from scratch and look for net conflicts."""
        problems = []
        grid = {}
        def stamp(layers, cx, cy, w, h, inflate, net, what):
            x0, x1 = cx - w / 2 - inflate, cx + w / 2 + inflate
            y0, y1 = cy - h / 2 - inflate, cy + h / 2 + inflate
            for l in layers:
                for gx in range(int(np.floor(x0 / GRID)), int(np.ceil(x1 / GRID)) + 1):
                    for gy in range(int(np.floor(y0 / GRID)), int(np.ceil(y1 / GRID)) + 1):
                        k = (l, gx, gy)
                        if k in grid and grid[k][0] != net:
                            problems.append((what, grid[k][1], l, gx * GRID, gy * GRID,
                                             net, grid[k][0]))
                        else:
                            grid[k] = (net, what)
        return problems, stamp, grid
