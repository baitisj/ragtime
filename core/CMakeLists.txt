# ragtime_core: the socket-free engines (protocol records, Data2G link,
# file transfer, broadcast repair, text coding, Globus projection, the
# gearbox model). No wxWidgets here: everything in core is unit-tested
# under ctest without a display.

configure_file(Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/ragtime/Version.h @ONLY)

add_library(ragtime_core STATIC
    src/Version.cpp
    # Package B: protocol v0, the Data2G link, session files, HamText, SHA-256.
    src/files/SessionFiles.cpp
    # Package G: the Files tab's rows and log, session files in a session.
    src/files/FileRows.cpp
    src/files/FileTraffic.cpp
    # Parity repairs: Cauchy Reed-Solomon over GF(2^16).
    src/files/Parity.cpp
    src/files/Sha256.cpp
    src/link/Data2GLink.cpp
    src/link/HostLink.cpp
    src/link/Reconnect.cpp
    src/protocol/Fields.cpp
    src/protocol/Reactions.cpp
    src/protocol/Records.cpp
    src/protocol/Stream.cpp
    src/text/HamText.cpp
    # Package C: Globus, the globe (projection, Natural Earth tables, light).
    src/globus/Globe.cpp
    src/globus/Sun.cpp
    # Package D (gearbox model): src/gearbox/.
    src/gearbox/Gearbox.cpp
    src/gearbox/Json.cpp
    src/gearbox/Submodes.cpp
    # Package E: the station list and the Broadcast tab's engine.
    src/broadcast/BroadcastChat.cpp
    src/broadcast/BroadcastLog.cpp
    src/broadcast/Stations.cpp
    src/broadcast/WatchList.cpp
    # Package H: broadcast files, seals and repair.
    src/broadcast/Airtime.cpp
    src/broadcast/BroadcastFiles.cpp
    src/broadcast/BroadcastParts.cpp
    src/broadcast/Seals.cpp
    # Package F (sessions): src/session/.
    src/session/SessionChat.cpp
    src/session/StreamLog.cpp
    src/session/SessionLog.cpp
    # The radio's dial from rigctld (read only), the US data segments, and
    # the path log: src/radio/.
    src/radio/Bands.cpp
    src/radio/DialReader.cpp
    src/radio/Noise.cpp
    src/radio/PathLog.cpp
    src/radio/RecentWork.cpp
    # Rig control: the Rig page's settings, and keying at data2g-host's PTT.
    src/radio/PttFollower.cpp
    src/radio/RigFilter.cpp
    src/radio/RigSettings.cpp
    src/radio/TxMeters.cpp
    src/radio/AutoAlc.cpp
    # The flight recorder: the host link on disk, for reports and --replay.
    src/link/FlightRecorder.cpp
    # The Ear: the spectrum, and finding data2g-host's sound card in
    # PipeWire's graph (the listening itself is in audio/).
    src/ear/AutoSpan.cpp
    src/ear/Edges.cpp
    src/ear/FilterControl.cpp
    src/ear/Graph.cpp
    src/ear/ProcNet.cpp
    src/ear/Spectrum.cpp
    # Voice messages: the microphones and speakers, the radio's card left
    # out (the recording and playing are in audio/).
    src/voice/Devices.cpp
    src/voice/MicCheck.cpp
    # Inline pictures: the detail-preserving downscaler, orientation and
    # what fits the air (the codecs are in image/).
    src/picture/Picture.cpp
)

target_include_directories(ragtime_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)
ragtime_warnings(ragtime_core)
