# The sotto vocoder Ragtime's voice messages are coded with: codec 1 is
# sotto v3.6, at this commit (docs/PROTOCOL.md, "Voice"). Ragtime loads
# libsotto at run time and never links it (LGPL through Codec 2); the
# packages carry it beside ragtime with its licences. Sourced by
# build-sotto.sh.
SOTTO_REPO=${SOTTO_REPO:-https://github.com/baitisj/sotto.git}
SOTTO_COMMIT=08d5047abb89c57cde03a5d9ca348b904460bb44
