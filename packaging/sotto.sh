# The sotto vocoder Ragtime's voice messages are coded with: codec 2 is
# sotto v3.8 (docs/PROTOCOL.md, "Voice"), 16 kHz speech with the 4-8 kHz
# hiss band, and the same library plays codec 1 (v3.6's frames) at 8 kHz.
# From ebe5386 it also has sotto 1200 (codec 3, sotto_lr_*) and sotto 600
# and 400's encoder (sotto_vlr_*); Ragtime sends 600 and 400 (codecs 4
# and 5) once a pinned commit has their float decoder too. Its v3.8 frames
# and sound are bit-identical to 594ca65's.
# Ragtime needs a library whose sotto_format() says "sotto v3.8, shape
# 9-9-8-8", so the variant is named here rather than left to sotto's
# Makefile. From this commit sotto's generated C tables are committed, so
# the build needs no Python. Ragtime loads libsotto at run time and never
# links it (LGPL through Codec 2); the packages carry it beside ragtime with
# its licences. Sourced by build-sotto.sh.
SOTTO_REPO=${SOTTO_REPO:-https://github.com/baitisj/sotto.git}
SOTTO_COMMIT=ebe5386cd704d2f5392bcd236a24d6683438bd6f
SOTTO_VARIANT=v3.8
