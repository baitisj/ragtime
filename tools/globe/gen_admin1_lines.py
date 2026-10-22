#!/usr/bin/env python3
"""Writes core/src/globus/Admin1Lines.h, the state and province lines Globus
draws thin and light over the land.

The source is Natural Earth's 1:10m admin-1 boundary lines (public domain,
https://www.naturalearthdata.com/), ne_10m_admin_1_states_provinces_lines
from https://github.com/nvkelso/natural-earth-vector/tree/master/geojson
(21 MB, so not kept here). Only the countries in COUNTRIES are taken: the
1:50m set has only 17 of Canada's 31 lines, British Columbia's line with
the Northwest Territories among those missing. Only the standard library
is needed:

    python3 tools/globe/gen_admin1_lines.py path/to/ne_10m_admin_1_states_provinces_lines.geojson --write

Pieces meeting end to end are joined, and each line is smoothed (Douglas-Peucker, to SMOOTH_DEGREES of latitude,
longitude shrunk by the cosine of the latitude), as the country borders
are, so the set stays small.

Three kinds of stretch are then left out, so every line Globus draws is
drawn once and only over land:
- along a border between countries, which Globus already draws in the
  heavier border line (the Saint John River, where Maine meets New
  Brunswick, is in both sets), read from core/src/globus/BorderLines.h;
- over a big lake (ne_110m_lakes.geojson beside this script), which
  Globus fills in the sea's colour: the Alberta and Saskatchewan line
  crosses Lake Athabasca;
- over the sea, by the land of core/src/globus/LandOutlines.h: Natural
  Earth carries some lines on through the Arctic channels and out to sea.

Long edges are split as for the coastlines, so that drawn as great circles
they still follow the map's straight lines (the 49th and 60th parallels,
for two), and the points go in hundredths of a degree.
"""

import argparse
import json
import math
import os
import re
import sys
import textwrap

from gen_land_outlines import MAX_EDGE_DEGREES, SCALE
from gen_lake_outlines import read_lakes

HERE = os.path.dirname(os.path.abspath(__file__))
LAKES = os.path.join(HERE, "ne_110m_lakes.geojson")
TABLES = os.path.join(HERE, "..", "..", "core", "src", "globus")
BORDERS = os.path.join(TABLES, "BorderLines.h")
LAND = os.path.join(TABLES, "LandOutlines.h")
OUT = os.path.join(TABLES, "Admin1Lines.h")

COUNTRIES = [
    "United States of America",
    "Canada",
    "Mexico",
    "Russia",
    "China",
    "India",
    "Brazil",
    "Argentina",
    "Australia",
]

SMOOTH_DEGREES = 0.25

# A stretch doubles a country border when every point along it, checked at
# SAMPLES places, is within BORDER_DEGREES of one. The borders are smoothed
# to 0.25 degrees, so this is a little wider. A stretch is checked against
# the land and the lakes at the middles of SAMPLES pieces, and only the
# pieces over water go.
BORDER_DEGREES = 0.35
SAMPLES = 8


def table(path, start_name):
    """A generated table's lines or rings, as lists of (lon, lat) in degrees."""
    with open(path) as f:
        text = f.read()

    def numbers(name):
        found = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
        return [int(v) for v in re.findall(r"-?\d+", found.group(1))]

    starts = numbers(start_name)
    values = numbers("POINTS")
    return [[(values[2 * k] / SCALE, values[2 * k + 1] / SCALE) for k in range(starts[i], starts[i + 1])]
            for i in range(len(starts) - 1)]


class Rings:
    """Rings with their bounding boxes, for asking whether a point is inside any, by the non-zero rule."""

    def __init__(self, rings):
        self.rings = []
        for ring in rings:
            xs = [p[0] for p in ring]
            ys = [p[1] for p in ring]
            self.rings.append((min(xs), max(xs), min(ys), max(ys), list(zip(ring, ring[1:] + ring[:1]))))

    def __contains__(self, point):
        x, y = point
        winding = 0
        for x0, x1, y0, y1, edges in self.rings:
            if x < x0 or x > x1 or y < y0 or y > y1:
                continue
            for (ax, ay), (bx, by) in edges:
                side = (bx - ax) * (y - ay) - (x - ax) * (by - ay)
                if ay <= y < by and side > 0:
                    winding += 1
                elif by <= y < ay and side < 0:
                    winding -= 1
        return winding != 0


def scaled(point, c):
    return (point[0] * c, point[1])


def distance_to(point, a, b):
    """Degrees from point to the segment ab, longitude shrunk by the cosine of the latitude."""
    c = math.cos(math.radians(point[1]))
    ax, ay = (a[0] - point[0]) * c, a[1] - point[1]
    bx, by = (b[0] - point[0]) * c, b[1] - point[1]
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = 0.0 if length == 0.0 else max(0.0, min(1.0, -(ax * dx + ay * dy) / length))
    return math.hypot(ax + dx * t, ay + dy * t)


def smoothed(line, tolerance):
    """Douglas-Peucker: the points kept, its ends always."""
    if len(line) <= 2:
        return list(line)
    c = math.cos(math.radians(sum(p[1] for p in line) / len(line)))
    keep = [False] * len(line)
    keep[0] = keep[-1] = True
    stack = [(0, len(line) - 1)]
    while stack:
        first, last = stack.pop()
        a, b = scaled(line[first], c), scaled(line[last], c)
        farthest, at = 0.0, None
        for i in range(first + 1, last):
            p = scaled(line[i], c)
            dx, dy = b[0] - a[0], b[1] - a[1]
            length = dx * dx + dy * dy
            t = 0.0 if length == 0.0 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length))
            d = math.hypot(a[0] + dx * t - p[0], a[1] + dy * t - p[1])
            if d > farthest:
                farthest, at = d, i
        if at is not None and farthest > tolerance:
            keep[at] = True
            stack.append((first, at))
            stack.append((at, last))
    return [p for p, k in zip(line, keep) if k]


def densified(line):
    """The open line with long edges split."""
    out = [tuple(line[0])]
    for (x0, y0), (x1, y1) in zip(line, line[1:]):
        pieces = max(1, math.ceil(max(abs(x1 - x0), abs(y1 - y0)) / MAX_EDGE_DEGREES))
        for k in range(1, pieces):
            t = k / pieces
            out.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
        out.append((x1, y1))
    return out


def read_lines(path):
    """The countries' admin-1 lines as lists of (lon, lat), cut where they cross the date line."""
    with open(path) as f:
        collection = json.load(f)
    lines = []
    for feature in collection["features"]:
        if feature["properties"].get("ADM0_NAME") not in COUNTRIES:
            continue
        geometry = feature["geometry"]
        parts = geometry["coordinates"] if geometry["type"] == "MultiLineString" else [geometry["coordinates"]]
        for part in parts:
            line = [tuple(p[:2]) for p in part]
            run = [line[0]]
            for a, b in zip(line, line[1:]):
                if abs(b[0] - a[0]) > 180.0:
                    lines.append(run)
                    run = []
                run.append(b)
            lines.append(run)
    return [line for line in lines if len(line) >= 2]


def joined(lines):
    """The lines with those that meet end to end, where no third line meets
    them, made one: Natural Earth cuts its lines into many short pieces, and
    a piece too short to smooth would keep every point."""
    def key(point):
        return (round(point[0], 6), round(point[1], 6))

    ends = {}
    for i, line in enumerate(lines):
        for point in (line[0], line[-1]):
            ends.setdefault(key(point), []).append(i)
    used = [False] * len(lines)

    def follow(run, at):
        # Extends run from its last point while exactly one other line ends there.
        while True:
            others = [j for j in ends[key(run[-1])] if not used[j]]
            if len(ends[key(run[-1])]) != 2 or len(others) != 1:
                return run
            j = others[0]
            used[j] = True
            line = lines[j] if key(lines[j][0]) == key(run[-1]) else lines[j][::-1]
            run.extend(line[1:])

    out = []
    for i, line in enumerate(lines):
        if used[i]:
            continue
        used[i] = True
        run = follow(list(line), i)
        run = follow(run[::-1], i)
        out.append(run)
    return out


def kept_runs(line, borders, land, lakes, left_out):
    """The runs of the line left once stretches along a border, over a lake or
    over the sea are taken out, counting the pieces taken out in left_out."""
    xs = [p[0] for p in line]
    ys = [p[1] for p in line]
    box = (min(xs) - 1.0, max(xs) + 1.0, min(ys) - 1.0, max(ys) + 1.0)
    near = [s for s in borders
            if box[0] <= max(s[0][0], s[1][0]) and min(s[0][0], s[1][0]) <= box[1]
            and box[2] <= max(s[0][1], s[1][1]) and min(s[0][1], s[1][1]) <= box[3]]

    def dry(point):
        return point in land and point not in lakes

    def doubles(a, b):
        along = [(a[0] + (b[0] - a[0]) * k / SAMPLES, a[1] + (b[1] - a[1]) * k / SAMPLES) for k in range(SAMPLES + 1)]
        return bool(near) and all(min(distance_to(p, *s) for s in near) <= BORDER_DEGREES for p in along)

    runs, run = [], [line[0]]
    for a, b in zip(line, line[1:]):
        if doubles(a, b):
            pieces = [(a, b, False)]
        elif dry(((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)) and dry(a) and dry(b):
            pieces = [(a, b, True)]
        else:
            cuts = [(a[0] + (b[0] - a[0]) * k / SAMPLES, a[1] + (b[1] - a[1]) * k / SAMPLES) for k in range(SAMPLES + 1)]
            cuts[0], cuts[-1] = a, b
            pieces = [(p, q, dry(((p[0] + q[0]) / 2, (p[1] + q[1]) / 2))) for p, q in zip(cuts, cuts[1:])]
        for p, q, keep in pieces:
            if not keep:
                left_out[0] += 1
                if len(run) >= 2:
                    runs.append(run)
                run = [q]
            else:
                if run[-1] != p:
                    if len(run) >= 2:
                        runs.append(run)
                    run = [p]
                run.append(q)
    if len(run) >= 2:
        runs.append(run)
    # Pieces of one stretch kept in a row are one straight run: only their
    # ends are needed.
    return [smoothed(r, 1e-9) for r in runs]


def header(lines):
    starts = []
    values = []
    for line in lines:
        starts.append(len(values) // 2)
        for lon, lat in line:
            values.append(int(round(lon * SCALE)))
            values.append(int(round(lat * SCALE)))
    starts.append(len(values) // 2)

    def wrap(numbers, per_line):
        rows = []
        for i in range(0, len(numbers), per_line):
            rows.append("    " + ", ".join(str(v) for v in numbers[i:i + per_line]) + ",")
        return "\n".join(rows)

    about = textwrap.fill(f"{len(lines)} lines, smoothed to {SMOOTH_DEGREES} degrees, for {', '.join(COUNTRIES)}.",
                          width=72, initial_indent="// ", subsequent_indent="// ")
    return f"""//=========================================================================
// Name:            Admin1Lines.h
// Purpose:         The state and province lines on Globus. Generated by
//                  tools/globe/gen_admin1_lines.py from Natural Earth's
//                  1:10m admin-1 boundary lines (public domain); do not
//                  edit by hand.
//
{about}
// None runs along a country border, over a big lake or over the sea. Line l
// is POINTS[LINE_START[l]] up to LINE_START[l + 1], each point a longitude
// then a latitude in hundredths of a degree.
//=========================================================================

#ifndef RAGTIME_GLOBUS_ADMIN1_LINES_H
#define RAGTIME_GLOBUS_ADMIN1_LINES_H

#include <cstdint>

namespace Admin1Lines
{{

constexpr int LINE_COUNT = {len(lines)};
constexpr int POINT_COUNT = {len(values) // 2};
constexpr double UNITS_PER_DEGREE = {SCALE}.0;

constexpr uint32_t LINE_START[LINE_COUNT + 1] = {{
{wrap(starts, 12)}
}};

constexpr int16_t POINTS[POINT_COUNT * 2] = {{
{wrap(values, 16)}
}};

}} // namespace Admin1Lines

#endif // RAGTIME_GLOBUS_ADMIN1_LINES_H
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("geojson", help="ne_10m_admin_1_states_provinces_lines.geojson")
    parser.add_argument("--write", action="store_true", help="write Admin1Lines.h rather than report")
    args = parser.parse_args()

    raw = joined(read_lines(args.geojson))
    raw_points = sum(len(line) for line in raw)
    borders = [s for line in table(BORDERS, "LINE_START") for s in zip(line, line[1:])]
    land = Rings(table(LAND, "RING_START"))
    lakes = Rings([ring for _, ring in read_lakes(LAKES)])

    left_out = [0]
    lines = []
    for line in raw:
        line = densified(smoothed(line, SMOOTH_DEGREES))
        lines.extend(kept_runs(line, borders, land, lakes, left_out))
    lines = [densified(line) for line in lines]
    text = header(lines)
    points = sum(len(line) for line in lines)
    print(f"{len(raw)} lines of {raw_points} points in, smoothed; {left_out[0]} pieces along a country border "
          f"or over water left out; {len(lines)} lines, {points} points, {len(text)} bytes of header", file=sys.stderr)
    if args.write:
        with open(OUT, "w") as f:
            f.write(text)
        print(f"wrote {os.path.normpath(OUT)}", file=sys.stderr)


if __name__ == "__main__":
    main()
