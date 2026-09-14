#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "CrossPointSettings.h"

/**
 * Get the user-configured daily reading goal in milliseconds.
 */
inline uint64_t getDailyReadingGoalMs() { return SETTINGS.getDailyGoalMs(); }

/**
 * Daily reading statistics for a single book.
 * 
 * Represents total reading time on a specific calendar day (UTC).
 * Note: No wall-clock time breakdown is available here;
 * see ReadingPeriodDayStats for time-of-day data.
 */
struct ReadingDayStats {
  uint32_t dayOrdinal = 0;  /**< Days since epoch (Jan 1, 1970 UTC) */
  uint64_t readingMs = 0;   /**< Total reading time in milliseconds */
};

/**
 * Daily reading statistics with time-of-day breakdown.
 * 
 * Extends ReadingDayStats by segmenting daily reading into three periods:
 * - Morning: 06:00-12:59 (often used for commute/breakfast reading)
 * - Afternoon: 13:00-20:59 (work day afternoon breaks)
 * - Night: 21:00-05:59 (evening/night reading)
 * 
 * Used for detailed reading analytics and trends over time.
 */
struct ReadingPeriodDayStats {
  uint32_t dayOrdinal = 0;    /**< Days since epoch (UTC) */
  uint32_t morningMs = 0;     /**< Reading time 06:00-12:59 */
  uint32_t afternoonMs = 0;   /**< Reading time 13:00-20:59 */
  uint32_t nightMs = 0;       /**< Reading time 21:00-05:59 */
};

/**
 * Complete reading statistics for a single book.
 * 
 * Tracks:
 * - Cumulative reading time (total and per-session)
 * - Progress through the book (current and chapter level)
 * - Completion status and timestamp
 * - Session count and duration
 * - Daily breakdown of reading activity
 * 
 * Used to populate the Reading Stats screen and achievement tracking.
 */
struct ReadingBookStats {
  std::string bookId;                         /**< Unique identifier (ISBN, EPUB id, hash) */
  std::string path;                           /**< Current file path on SD card */
  std::vector<std::string> knownPaths;        /**< Alternative paths (moved/renamed files) */
  std::string title;                          /**< Display title */
  std::string author;                         /**< Author name */
  std::string coverBmpPath;                   /**< Cached cover image path */
  std::string chapterTitle;                   /**< Current chapter name */
  std::vector<ReadingDayStats> readingDays;           /**< Daily totals without time-of-day */
  std::vector<ReadingPeriodDayStats> timedReadingDays; /**< Daily totals with time periods */
  uint64_t totalReadingMs = 0;                /**< Total cumulative reading time */
  uint32_t sessions = 0;                      /**< Number of reading sessions */
  uint32_t lastSessionMs = 0;                 /**< Duration of most recent session */
  uint32_t firstReadAt = 0;                   /**< Unix timestamp of first read */
  uint32_t lastReadAt = 0;                    /**< Unix timestamp of most recent read */
  uint32_t completedAt = 0;                   /**< Unix timestamp when book completed (0 if not) */
  uint8_t lastProgressPercent = 0;            /**< Current progress through book (0-100) */
  uint8_t chapterProgressPercent = 0;         /**< Current chapter progress (0-100) */
  bool completed = false;                     /**< Whether book has been finished */
};

/**
 * Snapshot of the current reading session in progress.
 * 
 * Captures session metadata at specific points (start, current, end).
 * Used by the device to track reading streaks and session statistics
 * without persisting to disk on every page turn.
 */
struct ReadingSessionSnapshot {
  bool valid = false;                  /**< Whether this snapshot contains valid data */
  uint32_t serial = 0;                 /**< Session serial number (incremented per new session) */
  std::string bookId;                  /**< Current book ID */
  std::string path;                    /**< Current book path */
  uint32_t sessionMs = 0;              /**< Reading time accumulated in this session */
  bool counted = false;                /**< Whether this session is counted toward stats */
  bool completedThisSession = false;   /**< Whether book was finished in this session */
  uint8_t startProgressPercent = 0;    /**< Progress % at session start */
  uint8_t endProgressPercent = 0;      /**< Progress % at session end */
};

/**
 * Historical log entry for a reading session.
 * 
 * Persisted to reading_log.jsonl for detailed analytics and manual correction.
 * Supports both legacy (untimed) and wall-clock timed entries.
 */
struct ReadingSessionLogEntry {
  uint32_t dayOrdinal = 0;             /**< Calendar day (days since epoch UTC) */
  uint32_t sessionMs = 0;              /**< Reading duration in milliseconds */
  std::string bookId;                  /**< Book identifier */
  std::string path;                    /**< Book file path */

  /**
   * Wall-clock timestamps for session boundaries.
   * Zero means legacy/untimed data. Never fabricate timestamps.
   */
  uint32_t startAt = 0;                /**< Unix timestamp of session start (or 0 if unknown) */
  uint32_t endAt = 0;                  /**< Unix timestamp of session end (or 0 if unknown) */

  /**
   * Time-of-day breakdown for timed sessions.
   * Legacy/untimed sessions have all zeros.
   */
  uint32_t morningMs = 0;              /**< Reading time during 06:00-12:59 */
  uint32_t afternoonMs = 0;            /**< Reading time during 13:00-20:59 */
  uint32_t nightMs = 0;                /**< Reading time during 21:00-05:59 */
};

class ReadingStatsStore;
namespace JsonSettingsIO {
bool saveReadingStats(const ReadingStatsStore& store, const char* path);
bool loadReadingStats(ReadingStatsStore& store, const char* json);
bool loadReadingStatsFromFile(ReadingStatsStore& store, const char* path);
bool loadReadingStatsDocument(ReadingStatsStore& store, const JsonDocument& doc);
}  // namespace JsonSettingsIO

class ReadingStatsStore {
  static ReadingStatsStore instance;

  struct SummaryCache {
    bool valid = false;
    uint32_t referenceDayOrdinal = 0;
    uint32_t booksFinishedCount = 0;
    uint64_t totalReadingMs = 0;
    uint64_t todayReadingMs = 0;
    uint64_t recent7ReadingMs = 0;
    uint64_t recent30ReadingMs = 0;
    uint32_t currentStreakDays = 0;
    uint32_t maxStreakDays = 0;
    uint64_t goalReadingMs = 0;
  };

  struct SessionState {
    bool active = false;
    size_t bookIndex = 0;
    unsigned long lastInteractionMs = 0;
    uint64_t accumulatedMs = 0;
    uint8_t startProgressPercent = 0;
    bool startCompleted = false;

    // Timing is best-effort only when the real device clock is valid.
    uint32_t startTimestamp = 0;
    uint64_t morningMs = 0;
    uint64_t afternoonMs = 0;
    uint64_t nightMs = 0;
  };

  std::vector<ReadingBookStats> books;
  std::vector<ReadingDayStats> legacyReadingDays;
  std::vector<ReadingDayStats> readingDays;
  std::vector<ReadingSessionLogEntry> sessionLog;
  SessionState activeSession;
  ReadingSessionSnapshot lastSessionSnapshot;
  uint32_t sessionSerialCounter = 0;
  mutable SummaryCache summaryCache;
  mutable bool dirty = false;
  mutable unsigned long lastSaveMs = 0;
  mutable bool persistenceSuspended = false;
  mutable bool skippedSaveLogged = false;
  mutable bool internalBackupPrepared = false;

  friend bool JsonSettingsIO::saveReadingStats(const ReadingStatsStore&, const char*);
  friend bool JsonSettingsIO::loadReadingStats(ReadingStatsStore&, const char*);
  friend bool JsonSettingsIO::loadReadingStatsFromFile(ReadingStatsStore&, const char*);
  friend bool JsonSettingsIO::loadReadingStatsDocument(ReadingStatsStore&, const JsonDocument&);

  size_t findBookIndexByPath(const std::string& path) const;
  size_t findBookIndexByBookId(const std::string& bookId) const;
  size_t findLegacyMergeCandidate(const std::string& path, const std::string& title = "",
                                  const std::string& author = "") const;
  void mergeBookInto(ReadingBookStats& primary, const ReadingBookStats& duplicate);
  void normalizeBook(ReadingBookStats& book);
  void normalizeBooks();
  void rememberBookPath(ReadingBookStats& book, const std::string& path);
  void rememberBookIdAlias(ReadingBookStats& book, const std::string& bookId);
  size_t getOrCreateBookIndex(const std::string& path, const std::string& title, const std::string& author,
                              const std::string& coverBmpPath, const std::string& preferredBookId = "");
  void touchBook(size_t index);
  ReadingDayStats& getOrCreateReadingDay(uint32_t epochSeconds);
  ReadingDayStats& getOrCreateBookReadingDay(ReadingBookStats& book, uint32_t epochSeconds);
  ReadingPeriodDayStats& getOrCreateBookTimedReadingDay(ReadingBookStats& book, uint32_t dayOrdinal);
  uint32_t getLatestKnownTimestamp() const;
  uint32_t getReferenceTimestamp(uint32_t preferredTimestamp, uint32_t bookTimestamp = 0) const;
  uint32_t getReferenceDayOrdinal() const;
  void updateBookReadTimestamp(ReadingBookStats& book, uint32_t preferredTimestamp);
  void recordReadingTime(ReadingBookStats& book, uint32_t epochSeconds, uint64_t readingMs);
  void recordTimedReading(ReadingBookStats& book, uint32_t endTimestamp, uint64_t readingMs);
  void appendSessionLogEntry(uint32_t dayOrdinal, uint32_t sessionMs, const ReadingBookStats& book,
                             uint32_t startAt = 0, uint32_t endAt = 0, uint32_t morningMs = 0,
                             uint32_t afternoonMs = 0, uint32_t nightMs = 0);
  bool convertLegacyReadingDaysToUnassigned();
  void rebuildAggregatedReadingDays();
  bool removeIgnoredBooks();
  bool hasAnyStats() const;
  void invalidateSummaryCache();
  void rebuildSummaryCache() const;
  bool shouldSaveDeferred() const;
  void markDirty();
  bool prepareInternalBackup() const;
  bool refreshInternalBackupFromMain() const;
  bool restoreInternalBackupToMain(const char* reason) const;
  bool maybeCreateAutoBackup(bool force) const;
  bool persistToFile(const char* path) const;
  static bool isClockValid(uint32_t epochSeconds);

 public:
  ~ReadingStatsStore() = default;

  static ReadingStatsStore& getInstance() { return instance; }

  void beginSession(const std::string& path, const std::string& title, const std::string& author,
                    const std::string& coverBmpPath, uint8_t progressPercent = 0, const std::string& chapterTitle = "",
                    uint8_t chapterProgressPercent = 0);
  void noteActivity();
  void tickActiveSession();
  void resumeSession();
  void updateProgress(uint8_t progressPercent, bool completed = false, const std::string& chapterTitle = "",
                      uint8_t chapterProgressPercent = 0);
  void endSession();
  bool adjustBookReadingTime(const std::string& path, uint32_t dayOrdinal, int32_t deltaMs);
  bool importExternalReadingStats(const std::string& path, const std::string& title, const std::string& author,
                                  uint32_t dayOrdinal, uint64_t readingMs, uint32_t sessions,
                                  uint64_t morningMs = 0, uint64_t afternoonMs = 0, uint64_t nightMs = 0,
                                  uint32_t detailedSessions = 0);
  bool importExternalReadingSession(const std::string& path, uint32_t startAt, uint32_t endAt, uint32_t sessionMs,
                                    uint32_t morningMs, uint32_t afternoonMs, uint32_t nightMs);
  bool hasTimedSession(const std::string& path, uint32_t startAt) const;
  bool setBookFirstReadDate(const std::string& path, uint32_t dayOrdinal);
  bool updateBookMetadata(const std::string& path, const std::string& title, const std::string& author,
                          const std::string& coverBmpPath);
  bool updateBookPath(const std::string& oldKey, const std::string& newPath, const std::string& title = "",
                      const std::string& author = "", const std::string& coverBmpPath = "",
                      const std::string& bookId = "");
  bool removeBook(const std::string& path);
  const ReadingBookStats* findBook(const std::string& key) const;
  const ReadingBookStats* findMatchingBookForPath(const std::string& path, const std::string& title = "",
                                                  const std::string& author = "") const;
  const ReadingSessionSnapshot& getLastSessionSnapshot() const { return lastSessionSnapshot; }

  const std::vector<ReadingBookStats>& getBooks() const { return books; }
  const std::vector<ReadingDayStats>& getReadingDays() const { return readingDays; }
  const std::vector<ReadingSessionLogEntry>& getSessionLog() const { return sessionLog; }
  static bool shouldIgnorePath(const std::string& path);

  uint32_t getBooksStartedCount() const { return static_cast<uint32_t>(books.size()); }
  uint32_t getBooksFinishedCount() const;
  uint64_t getTotalReadingMs() const;
  uint64_t getTodayReadingMs() const;
  uint64_t getRecentReadingMs(uint32_t days) const;
  uint32_t getCurrentStreakDays() const;
  uint32_t getMaxStreakDays() const;
  uint32_t getDisplayTimestamp(bool* usedFallback = nullptr) const;
  bool hasReadingDays() const { return !readingDays.empty(); }

  void reset();
  bool exportToFile(const std::string& path) const;
  bool importFromFile(const std::string& path);
  bool saveToFile() const;
  bool isAutoBackupDue() const;
  bool createDueAutoBackup() const;
  bool hasAutoBackups() const;
  bool ensureAutoBackupForEnabledSetting() const;
  int clearAutoBackups() const;
  bool loadFromFile();
  void markLoadSkippedForRecovery();
  bool releaseMemoryForNetwork();
  bool reloadAfterNetwork();
};

#define READING_STATS ReadingStatsStore::getInstance()
