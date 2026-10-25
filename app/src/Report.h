#ifndef RAGTIME_REPORT_H
#define RAGTIME_REPORT_H

#include <string>

#include "ragtime/link/FlightRecorder.h"

class wxWindow;

// What the window knows that a report should say.
struct ReportFacts
{
    std::string callsign;
    std::string host;           // data2g-host's address
    std::string hostState;      // "Connected", "Reconnecting in 8 s"
    std::string lastError;      // why the last try failed
};

// Help > Save a report (docs/FLIGHT_RECORDER.md): asks where, then zips
// the flight recorder's files with the settings and a few facts about this
// Ragtime, so a tester sends a file instead of a recollection. Settings
// named like a secret (link::reportableSetting) are left out, so a private
// key never goes in one.
void saveReport(wxWindow* parent, ragtime::link::FlightRecorder& recorder, const ReportFacts& facts);

#endif
