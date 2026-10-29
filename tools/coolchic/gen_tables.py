# gen_tables.py TABLES.txt OUT.h: writes the range coder's two integer
# tables (docs/PICTURE_CODEC.md, "Frequencies") as a C++ header, from
# tools/codecs/rcc/tables.txt, which tables.py makes with exact decimal
# arithmetic. The C++ takes them as they are, so every station has the
# same frequencies.
import sys

src, out = sys.argv[1:3]
k, p = [], []
for line in open(src):
    if line.startswith("#") or not line.strip():
        continue
    name, i, v = line.split()
    (k if name == "K" else p).append(int(v))
assert len(k) == 2561 and len(p) == 1024


def rows(values):
    return "\n".join(
        "    " + " ".join(f"{v}u," for v in values[i:i + 6]) for i in range(0, len(values), 6)
    )


with open(out, "w") as f:
    f.write(
        "// The range coder's tables (docs/PICTURE_CODEC.md, \"Frequencies\"), written by\n"
        "// tools/coolchic/gen_tables.py from tools/codecs/rcc/tables.txt. Do not edit.\n"
        "//\n"
        "//   K[s] = round(2^32 / (256 * exp(-5 + s/256) * ln 2)), s = 0..2560\n"
        "//   P[f] = round(2^31 * 2^(-f/1024)), f = 0..1023\n"
        "#ifndef RAGTIME_IMAGE_COOLCHIC_RANGE_TABLES_H\n"
        "#define RAGTIME_IMAGE_COOLCHIC_RANGE_TABLES_H\n\n"
        "#include <cstdint>\n\n"
        "namespace ragtime\n{\nnamespace image\n{\nnamespace coolchic\n{\n\n"
        f"constexpr int SCALE_TABLE_SIZE = {len(k)};\n"
        f"constexpr int POW_TABLE_SIZE = {len(p)};\n\n"
        f"const uint32_t SCALE_TABLE[SCALE_TABLE_SIZE] = {{\n{rows(k)}\n}};\n\n"
        f"const uint32_t POW_TABLE[POW_TABLE_SIZE] = {{\n{rows(p)}\n}};\n\n"
        "} // namespace coolchic\n} // namespace image\n} // namespace ragtime\n\n#endif\n"
    )
