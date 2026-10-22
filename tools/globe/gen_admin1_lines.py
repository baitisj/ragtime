#!/usr/bin/env python3
"""Writes core/src/globus/Admin1Lines.h, the US state and Canadian province
lines Globus draws thin and light over the land.

The source is us_canada_admin1_lines.json beside this script: Natural
Earth's admin-1 boundary lines (public domain,
https://www.naturalearthdata.com/) for the United States and Canada,
simplified to about a quarter of a degree, each line a list of
[longitude, latitude] points. Only the standard library is needed:

    python3 tools/globe/gen_admin1_lines.py --write

Two kinds of stretch are left out. One that runs along a border between
countries, which Globus already draws in the country borders' heavier
line (the Saint John River, where Maine meets New Brunswick and Quebec,
is in both sets), so a thin line never shadows the thick one. And one
over the water of a big lake (LakeOutlines.h, from ne_110m_lakes.geojson:
the Alberta and Saskatchewan line crosses Lake Athabasca, and the
Saskatchewan and Manitoba line Reindeer Lake),
which Globus fills in the sea's colour. The country borders are read
from the generated core/src/globus/BorderLines.h.

Long edges are split as for the coastlines, so that drawn as great circles
they still follow the map's straight lines (the 49th parallel, for one),
and the points go in hundredths of a degree.
"""

import argparse
import json
import math
import os
import re
import sys

from gen_land_outlines import MAX_EDGE_DEGREES, SCALE
from gen_lake_outlines import read_lakes

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "us_canada_admin1_lines.json")
LAKES = os.path.join(HERE, "ne_110m_lakes.geojson")
BORDERS = os.path.join(HERE, "..", "..", "core", "src", "globus", "BorderLines.h")
OUT = os.path.join(HERE, "..", "..", "core", "src", "globus", "Admin1Lines.h")

# A stretch doubles a country border when every point along it, checked at
# SAMPLES places, is within BORDER_DEGREES of one (longitude shrunk by the
# cosine of the latitude). The borders are smoothed to 0.25 degrees, so
# this is a little wider.
BORDER_DEGREES = 0.35
SAMPLES = 9


def read_borders(path):
    """The country borders as segments ((lon, lat), (lon, lat)), in degrees."""
    with open(path) as f:
        text = f.read()

    def numbers(name):
        found = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
        return [int(v) for v in re.findall(r"-?\d+", found.group(1))]

    starts = numbers("LINE_START")
    values = numbers("POINTS")
    segments = []
    for l in range(len(starts) - 1):
        points = [(values[2 * k] / SCALE, values[2 * k + 1] / SCALE) for k in range(starts[l], starts[l + 1])]
        segments.extend(zip(points, points[1:]))
    return segments


def distance_to(point, a, b):
    """Degrees from point to the segment ab, longitude shrunk by the cosine of the latitude."""
    c = math.cos(math.radians(point[1]))
    ax, ay = (a[0] - point[0]) * c, a[1] - point[1]
    bx, by = (b[0] - point[0]) * c, b[1] - point[1]
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = 0.0 if length == 0.0 else max(0.0, min(1.0, -(ax * dx + ay * dy) / length))
    return math.hypot(ax + dx * t, ay + dy * t)


def inside(point, ring):
    """Whether point is inside the ring, by the even-odd rule in longitude and latitude."""
    x, y = point
    result = False
    for (x0, y0), (x1, y1) in zip(ring, ring[1:] + ring[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            result = not result
    return result


def kept_runs(line, borders, lakes, left_out):
    """The runs of the line left once stretches along a border or over a lake are taken out,
    each stretch taken out named in left_out."""
    near = [s for s in borders if min(distance_to(p, *s) for p in line) < 5.0]

    def wet(point):
        return any(inside(point, ring) for ring in lakes)

    def doubles(a, b):
        along = [(a[0] + (b[0] - a[0]) * k / (SAMPLES - 1), a[1] + (b[1] - a[1]) * k / (SAMPLES - 1))
                 for k in range(SAMPLES)]
        return bool(near) and all(min(distance_to(p, *s) for s in near) <= BORDER_DEGREES for p in along)

    runs, run = [], [tuple(line[0])]
    for a, b in zip(line, line[1:]):
        a, b = tuple(a), tuple(b)
        if doubles(a, b):
            pieces = [(a, b, False)]
        else:
            # Where a stretch crosses a lake, only the part over the water
            # goes, to within a SAMPLES-th of the stretch.
            cuts = [(a[0] + (b[0] - a[0]) * k / SAMPLES, a[1] + (b[1] - a[1]) * k / SAMPLES) for k in range(SAMPLES + 1)]
            cuts[0], cuts[-1] = a, b
            pieces = [(p, q, not wet(((p[0] + q[0]) / 2, (p[1] + q[1]) / 2))) for p, q in zip(cuts, cuts[1:])]
            if all(keep for _, _, keep in pieces):
                pieces = [(a, b, True)]
        for p, q, keep in pieces:
            if not keep:
                why = "along a border" if len(pieces) == 1 else "over a lake"
                left_out.append(f"({p[0]:.2f}, {p[1]:.2f}) to ({q[0]:.2f}, {q[1]:.2f}) {why}")
            if keep:
                if run[-1] != p:
                    if len(run) >= 2:
                        runs.append(run)
                    run = [p]
                run.append(q)
            else:
                if len(run) >= 2:
                    runs.append(run)
                run = [q]
    if len(run) >= 2:
        runs.append(run)
    return runs


def densified(line):
    """The open line with long edges split."""
    out = [tuple(line[0])]
    for (x0, y0), (x1, y1) in zip(line, line[1:]):
        pieces = max(1, int(-(-max(abs(x1 - x0), abs(y1 - y0)) // MAX_EDGE_DEGREES)))
        for k in range(1, pieces):
            t = k / pieces
            out.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
        out.append((x1, y1))
    return out


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

    return f"""//=========================================================================
// Name:            Admin1Lines.h
// Purpose:         The US state and Canadian province lines on Globus.
//                  Generated by tools/globe/gen_admin1_lines.py from Natural
//                  Earth's admin-1 boundary lines (public domain); do not
//                  edit by hand.
//
// {len(lines)} lines. Line l is POINTS[LINE_START[l]] up to LINE_START[l + 1], each
// point a longitude then a latitude in hundredths of a degree.
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
    parser.add_argument("source", nargs="?", default=SOURCE)
    parser.add_argument("--write", action="store_true", help="write Admin1Lines.h rather than report")
    args = parser.parse_args()

    with open(args.source) as f:
        raw = json.load(f)
    borders = read_borders(BORDERS)
    lakes = [ring for _, ring in read_lakes(LAKES)]
    # Split first, so a long stretch that only crosses a lake loses just
    # the piece over the water.
    split = [densified(line) for line in raw if len(line) >= 2]
    left_out = []
    lines = [run for line in split for run in kept_runs(line, borders, lakes, left_out)]
    for stretch in left_out:
        print("left out", stretch, file=sys.stderr)
    text = header(lines)
    points = sum(len(line) for line in lines)
    print(f"{len(raw)} lines in, {len(left_out)} pieces along a country border or over a lake left out, "
          f"{len(lines)} lines, {points} points, {len(text)} bytes of header", file=sys.stderr)
    if args.write:
        with open(OUT, "w") as f:
            f.write(text)
        print(f"wrote {os.path.normpath(OUT)}", file=sys.stderr)


if __name__ == "__main__":
    main()
