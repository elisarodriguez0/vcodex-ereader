#pragma once

namespace SilentTimeSync {

// Schedule a silent saved-Wi-Fi/NTP refresh after normal boot has completed.
// This function never performs network work itself.
void schedule(bool allowWifiAttempt);

// Advance the post-boot sync state machine. It is fully non-blocking.
// Returns true only on the loop iteration where a successful NTP sync is
// persisted, so the caller can refresh visible date/time UI.
bool tick();

// Tell the background sync that the user is interacting with the device.
// Waiting work is postponed; active Wi-Fi/NTP work is cancelled and retried
// later so background time sync never competes with foreground interaction.
void notifyUserActivity();

bool isPendingOrRunning();

}  // namespace SilentTimeSync
