#pragma once

namespace KindleStatsBridge {

// Downloads the latest cumulative Kindle statistics snapshot from the same
// backend configured as the "Ereader Sync" OPDS server.
//
// This only stages the JSON on the SD card. It does not touch ReadingStatsStore
// while network memory is released.
bool downloadSnapshot();

// Imports the staged snapshot into VCodex ReadingStatsStore using a local
// cumulative baseline so repeated pulls never double-count Kindle time.
//
// Returns true when the snapshot was parsed and the baseline was processed.
// Missing local books are intentionally skipped without advancing their
// baseline, so their historical Kindle stats can be imported later if the book
// appears on the X4.
bool importDownloadedSnapshot();

}  // namespace KindleStatsBridge
