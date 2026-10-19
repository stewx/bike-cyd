#pragma once

#include <Arduino.h>

// Turns recorded ride CSVs into Strava-ready TCX files.
namespace tcx {

// Converts every ride in /rides/finished: rides long enough to keep become
// /rides/pending/<local start time>.tcx with the CSV archived in /rides/csv;
// short ones move to /rides/discarded, unreadable ones to /rides/failed.
// Returns the number of TCX files written.
int exportFinished();

}  // namespace tcx
