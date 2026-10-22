# Sourced by the packaging scripts: sets RAGTIME_VERSION (from the top-level
# CMakeLists.txt) and RAGTIME_RELEASE, the part every release file name
# shares, such as 0.1.0-beta-1a2b3c4. RAGTIME_RELEASE_TAG ("beta" unless
# set, and may be empty) names the release; the hash is the commit built.
RAGTIME_VERSION=$(sed -n 's/^project(ragtime VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
RAGTIME_RELEASE_TAG=${RAGTIME_RELEASE_TAG-beta}
RAGTIME_RELEASE=$RAGTIME_VERSION${RAGTIME_RELEASE_TAG:+-$RAGTIME_RELEASE_TAG}-$(git rev-parse --short=7 HEAD 2>/dev/null || echo nogit)
