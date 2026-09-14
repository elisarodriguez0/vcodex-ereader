#pragma once

#include <cstdint>
#include <string>

/**
 * Time and date utilities for the e-reader.
 * 
 * Handles:
 * - NTP time synchronization (non-blocking)
 * - System clock management from hardware RTC
 * - Timezone configuration
 * - Unix timestamp and calendar date conversions
 * - "Day ordinal" calculations for reading statistics (days since epoch UTC)
 * - Formatted time/date display for UI
 * - FAT filesystem metadata timestamp callbacks
 * 
 * Key Concepts:
 * - "Day ordinal": Number of days since Unix epoch (1970-01-01 UTC)
 * - "Authoritative timestamp": Last known good NTP-synced time
 * - "Best effort timestamp": Live clock if valid, otherwise last known good
 * - "Wall clock": Current time from RTC (may drift if not NTP-synced)
 */
namespace TimeUtils {

/**
 * Initialize timezone from device settings.
 * Call once at startup to configure system timezone for local time conversions.
 */
void configureTimezone();

/**
 * Stop NTP synchronization if in progress.
 * Called after syncTimeWithNtp() completes or on shutdown.
 */
void stopNtp();

/**
 * Start non-blocking NTP synchronization.
 * 
 * Initiates SNTP client to synchronize with NTP servers.
 * Use pollNtpSync() to check when synchronization completes.
 * Must call stopNtp() when finished.
 * 
 * Non-blocking: returns immediately without waiting for sync.
 */
void startNtpSync();

/**
 * Check if NTP synchronization has completed.
 * 
 * @return true if system time has been synchronized, false if still waiting
 */
bool pollNtpSync();

/**
 * Perform blocking NTP time synchronization.
 * 
 * Waits for NTP response with configurable timeout.
 * Blocks the calling task until sync succeeds or timeout expires.
 * 
 * @param timeoutMs Maximum time to wait for NTP response (default 5000ms)
 * @return true if sync succeeded, false on timeout or error
 */
bool syncTimeWithNtp(uint32_t timeoutMs = 5000);

/**
 * Check if the system clock is valid and synchronized.
 * 
 * A valid clock has been synchronized with NTP and is not too far in past.
 * 
 * @return true if current system time is considered valid
 */
bool isClockValid();

/**
 * Check if a specific timestamp is valid.
 * 
 * @param epochSeconds Unix timestamp to validate
 * @return true if timestamp is in reasonable range
 */
bool isClockValid(uint32_t epochSeconds);

/**
 * Get the most recently NTP-synchronized timestamp.
 * 
 * @return Last known good timestamp from NTP (may be from previous boot)
 */
uint32_t getAuthoritativeTimestamp();

/**
 * Get current time if clock is valid, otherwise last known good time.
 * 
 * Prefer this for user-visible timestamps when perfect accuracy isn't essential.
 * 
 * @return Current or best-known timestamp
 */
uint32_t getCurrentValidTimestamp();

/**
 * Get the best wall-clock time for SD card filesystem metadata.
 * 
 * Returns:
 * 1. Current live clock if NTP-synced and within reasonable range
 * 2. Otherwise, the last "Day Changed" timestamp (start of day)
 * 3. Otherwise, last known good NTP timestamp
 * 
 * This ensures file timestamps are monotonically increasing and correct
 * even if NTP hasn't synced this boot.
 */
uint32_t getBestEffortFileTimestamp();

/**
 * Register callback for SD FAT filesystem timestamp generation.
 * Used internally by SD card driver to stamp file metadata.
 * Call once at startup.
 */
void registerSdFatDateTimeCallback();

/**
 * Set the current date from user input.
 * Updates system time to midnight UTC on the given date.
 * 
 * @param year Calendar year
 * @param month Calendar month (1-12)
 * @param day Calendar day (1-31)
 * @param epochSeconds Output: Unix timestamp for midnight UTC on this date
 * @return true if date was valid and set, false otherwise
 */
bool setCurrentDate(int year, unsigned month, unsigned day, uint32_t* epochSeconds = nullptr);

/**
 * Convert a calendar date to Unix timestamp.
 * Returns the Unix timestamp for midnight UTC on the given date.
 * 
 * @param year Calendar year
 * @param month Calendar month (1-12)
 * @param day Calendar day (1-31)
 * @param epochSeconds Output: Unix timestamp
 * @return true if date was valid, false otherwise
 */
bool getTimestampForLocalDate(int year, unsigned month, unsigned day, uint32_t* epochSeconds);

/**
 * Get the "day ordinal" (days since epoch) for a given timestamp.
 * 
 * Day ordinal 0 = Jan 1, 1970 UTC
 * Day ordinal 1 = Jan 2, 1970 UTC
 * Used for reading statistics and daily tracking.
 * 
 * @param epochSeconds Unix timestamp
 * @return Number of days since epoch (0-based)
 */
uint32_t getLocalDayOrdinal(uint32_t epochSeconds);

/**
 * Get the day ordinal for a specific calendar date.
 * 
 * @param year Calendar year
 * @param month Calendar month (1-12)
 * @param day Calendar day (1-31)
 * @return Day ordinal (0 for Jan 1, 1970)
 */
uint32_t getDayOrdinalForDate(int year, unsigned month, unsigned day);

/**
 * Convert a day ordinal to calendar date.
 * 
 * @param dayOrdinal Days since epoch
 * @param year Output: calendar year
 * @param month Output: calendar month (1-12)
 * @param day Output: calendar day (1-31)
 * @return true if ordinal was valid, false otherwise
 */
bool getDateFromDayOrdinal(uint32_t dayOrdinal, int& year, unsigned& month, unsigned& day);

/**
 * Check whether time has been NTP-synced since device boot.
 * 
 * @return true if NTP sync occurred this boot, false if using RTC only
 */
bool wasTimeSyncedThisBoot();

/**
 * Get the label for the current timezone setting.
 * 
 * @return Timezone label (e.g., "UTC", "America/New_York")
 */
const char* getCurrentTimeZoneLabel();

/**
 * Format Unix timestamp as a date string.
 * 
 * @param epochSeconds Unix timestamp to format
 * @param appendBang Whether to append "!" if timestamp wasn't NTP-synced
 * @return Formatted date (e.g., "Jan 15, 2024")
 */
std::string formatDate(uint32_t epochSeconds, bool appendBang = false);

/**
 * Format Unix timestamp as a date and time string.
 * 
 * @param epochSeconds Unix timestamp to format
 * @param appendBang Whether to append "!" if timestamp wasn't NTP-synced
 * @return Formatted datetime (e.g., "Jan 15, 2024 14:30:45")
 */
std::string formatDateTime(uint32_t epochSeconds, bool appendBang = false);

/**
 * Format calendar date parts as a string.
 * 
 * @param year Calendar year
 * @param month Calendar month (1-12)
 * @param day Calendar day (1-31)
 * @param appendBang Whether to append "!"
 * @return Formatted date string
 */
std::string formatDateParts(int year, unsigned month, unsigned day, bool appendBang = false);

/**
 * Format year and month for display.
 * 
 * @param year Calendar year
 * @param month Calendar month (1-12)
 * @return Formatted month/year (e.g., "January 2024")
 */
std::string formatMonthYear(int year, unsigned month);

/**
 * Check if hardware RTC auto-day-increment is enabled.
 * When enabled, RTC date automatically advances at midnight.
 */
bool isHardwareRtcAutoDayClockActive();

/**
 * Format current time for status bar display.
 * 
 * @param buf Output buffer for formatted time
 * @param bufSize Buffer size in bytes
 * @param use12Hour Whether to use 12-hour (AM/PM) format
 * @return true if format succeeded, false if buffer too small
 */
bool formatStatusBarClockTime(char* buf, size_t bufSize, bool use12Hour);

/**
 * Sync system clock from hardware RTC.
 * 
 * Updates system time to match RTC register values.
 * May trigger a "day changed" event if RTC date advanced.
 * 
 * @param forceRefresh If true, refresh even if recently synced
 * @return true if sync succeeded, false on RTC error
 */
bool applySystemClockFromRtc(bool forceRefresh = false);

/**
 * Tick the system clock based on RTC incrementally.
 * Lower overhead than applySystemClockFromRtc(); called frequently
 * to keep system time loosely synchronized with RTC between full syncs.
 */
void tickSystemClockFromRtc();

}  // namespace TimeUtils
