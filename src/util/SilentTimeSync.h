#pragma once

namespace SilentTimeSync {

// Silently refreshes the system date/time from a saved Wi-Fi network when
// Auto Sync Day is enabled and the device does not already have today's
// trusted date. Returns true only after a successful NTP synchronization.
//
// This helper never opens the Wi-Fi selector and always shuts Wi-Fi down
// before returning. Failure is non-fatal.
bool run(bool allowWifiAttempt);

}  // namespace SilentTimeSync
