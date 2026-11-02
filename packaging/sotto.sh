# The sotto vocoder Ragtime's voice messages are coded with: codec 1 is
# sotto v3.6's frames (docs/PROTOCOL.md, "Voice"), built as the v3.7
# variant at this commit. v3.7 changes only the encoder (a pitch candidate
# at a third of Codec 2's estimate): its frames are v3.6's and its
# sotto_format() still says "sotto v3.6, shape 9-9-8-8". Name the variant:
# sotto's Makefile builds v3.8 by default, whose frames differ (a side
# channel in unvoiced frames) and which Ragtime refuses. Ragtime loads
# libsotto at run time and never links it (LGPL through Codec 2); the
# packages carry it beside ragtime with its licences. Sourced by
# build-sotto.sh.
SOTTO_REPO=${SOTTO_REPO:-https://github.com/baitisj/sotto.git}
SOTTO_COMMIT=a84c6f5c15a96f0e7d393802d53e4d230192a5d4
SOTTO_VARIANT=v3.7
