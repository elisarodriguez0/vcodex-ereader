#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * Metadata and state for a StarDict dictionary on the SD card.
 * 
 * StarDict is an open-source dictionary format (www.stardict.org).
 * Each dictionary is a directory containing:
 * - .ifo: Dictionary metadata (name, language, word count)
 * - .idx: Word list index (binary format, allows fast binary search)
 * - .dict: Definition data (compressed/uncompressed)
 * - .syn: Optional synonym mappings
 * 
 * Runtime State:
 * - Checkpoints and ordinals are cached after first use (mutable for lazy init)
 * - Definition cache path points to preprocessed definitions on SD card
 */
struct DictionaryEntry {
  std::string languageId;          /**< Language code (e.g., "en", "es", "de") */
  std::string directoryPath;       /**< Directory containing dictionary files */
  std::string ifoPath;             /**< Path to .ifo metadata file */
  std::string idxPath;             /**< Path to .idx index file */
  std::string dictPath;            /**< Path to .dict definition data file */
  std::string synPath;             /**< Path to .syn synonym file (if present) */
  std::string cachePath;           /**< Path to preprocessed definition cache */
  std::string name;                /**< Display name for the dictionary */
  std::string lang;                /**< Language description from metadata */
  std::string sameTypeSequence;    /**< StarDict format version indicator */
  uint32_t wordCount = 0;          /**< Number of headwords in dictionary */
  uint32_t idxFileSize = 0;        /**< Size of index file in bytes */
  bool compressed = false;         /**< Whether .dict file uses compression */
  bool missingFiles = false;       /**< Whether required files are missing on SD */
  
  /* Cached lookup structures (lazy-initialized on first search) */
  mutable std::vector<uint32_t> checkpoints;  /**< Index split points for binary search */
  mutable std::vector<uint32_t> ordinals;     /**< Word entry offsets in index */
  mutable uint32_t totalWords = 0;            /**< Total discoverable words */
};

/**
 * Result of a dictionary lookup operation.
 * 
 * Contains the definition (if found), suggestions for typos,
 * and status information.
 */
struct DictionaryLookupResult {
  /**
   * Lookup result status enumeration.
   */
  enum class Status {
    Found,         /**< Definition was found and returned */
    NotFound,      /**< Word not found in active dictionary */
    NoDictionary,  /**< No dictionary is currently active */
    NotReady       /**< Dictionary lookup not available (e.g., still preparing) */
  };

  Status status = Status::NotFound;           /**< Lookup result status */
  std::string query;                          /**< Original user query (normalized) */
  std::string headword;                       /**< Canonical headword in dictionary */
  std::string definition;                     /**< Definition text (possibly truncated) */
  std::string dictionaryName;                 /**< Name of dictionary that provided result */
  bool truncated = false;                     /**< Whether definition was cut off */
  std::vector<std::string> suggestions;       /**< Suggested words for typos (if NotFound) */
};

/**
 * Singleton manager for StarDict dictionary access and caching.
 * 
 * Handles:
 * - Discovery of .stardict/ dictionaries on the SD card
 * - Selection and activation of the current dictionary
 * - Binary-search lookup with fuzzy matching and suggestions
 * - Definition caching to reduce SD card I/O
 * - Text size preference and font ID mapping
 * - Lookup history for quick access to recently viewed words
 * 
 * Integration:
 * - Called by the Reader activity for word lookup (long-press)
 * - Configuration saved to settings.json
 * - Definitions cached in .stardict_cache/ on SD card
 */
class DictionaryStore {
 public:
  /**
   * Definition display text size preference enumeration.
   */
  enum DefinitionTextSize : uint8_t {
    DEF_TEXT_SMALL = 0,     /**< Compact definition display (fit more on screen) */
    DEF_TEXT_LARGE = 1,     /**< Large definition display (easier to read) */
    DEF_TEXT_SIZE_COUNT     /**< Enum size constant */
  };

  /**
   * Get the singleton instance of the dictionary store.
   */
  static DictionaryStore& getInstance();

  /**
   * Load dictionary configuration and preference from settings.json.
   * Call at startup before any dictionary operations.
   */
  void loadConfig();

  /**
   * Save dictionary configuration (active dictionary, text size) to settings.json.
   * 
   * @return true if save succeeded, false on I/O error
   */
  bool saveConfig() const;

  /**
   * Scan the SD card for available StarDict dictionaries in .stardict/ directory.
   * Should be called at startup and when returning from file uploads.
   */
  void scan();

  /**
   * Ensure dictionaries have been scanned at least once.
   * Automatically calls scan() if not yet initialized.
   */
  void ensureScanned();

  /**
   * Get all discovered dictionary entries.
   */
  const std::vector<DictionaryEntry>& getEntries() const { return entries; }

  /**
   * Get the index of the currently active dictionary.
   * 
   * @return Index into getEntries(), or -1 if no dictionary is active
   */
  int getActiveIndex() const { return activeIndex; }

  /**
   * Set the active dictionary by index.
   * Prepares the new dictionary for lookup operations.
   * 
   * @param index Index into getEntries()
   * @return true if activation succeeded, false if index is invalid
   */
  bool setActiveIndex(int index);

  /**
   * Get the display label for the active dictionary.
   * 
   * @return Dictionary name, or "No dictionary" if none active
   */
  std::string getActiveLabel() const;

  /**
   * Get the user's preferred text size for definition display.
   */
  uint8_t getDefinitionTextSize() const { return definitionTextSize; }

  /**
   * Set the user's preferred text size for definition display.
   * Changes are not automatically saved; call saveConfig() to persist.
   * 
   * @param size DEF_TEXT_SMALL or DEF_TEXT_LARGE
   * @return true if set successfully, false if size is invalid
   */
  bool setDefinitionTextSize(uint8_t size);

  /**
   * Get the font ID to use for displaying definitions.
   * Selects a smaller font variant of the reader font for compact display.
   * 
   * @param readerFontId Current reader font ID
   * @return Font ID to use for definitions
   */
  int getDefinitionFontId(int readerFontId) const;

  /**
   * Prepare the active dictionary for lookup (build index structures).
   * This can be slow (100-500ms) for large dictionaries; should be called
   * before the user can interact with the dictionary (e.g., during app init).
   * 
   * @param onProgress Optional callback to report preparation progress (0-100)
   * @return true if preparation succeeded, false if dictionary is corrupt/missing
   */
  bool prepareActive(const std::function<void(int percent)>& onProgress = nullptr);

  /**
   * Check whether an active dictionary is ready for use.
   * 
   * @return true if a dictionary is active and prepared, false otherwise
   */
  bool hasActiveDictionary() const;

  /**
   * Look up a word in the active dictionary.
   * 
   * Performs binary search on the index and retrieves definition.
   * If not found, generates suggestions for typos.
   * 
   * @param rawWord User-entered word (will be normalized)
   * @param includeSuggestions Whether to generate typo suggestions if not found
   * @return Lookup result with status, definition, and/or suggestions
   */
  DictionaryLookupResult lookup(const std::string& rawWord, bool includeSuggestions = true);

  /**
   * Get the history of recently looked up words.
   * Most recent lookups appear first.
   */
  std::vector<std::string> getHistory();

  /**
   * Add a word to the lookup history.
   * Used to track user interest for frequent word lists.
   * 
   * @param word Word to record (will be normalized)
   */
  void addHistory(const std::string& word);

  /**
   * Clear all lookup history.
   */
  void clearHistory();

  /**
   * Normalize a word for dictionary lookup.
   * Applies lowercasing, accent stripping, and special character handling.
   * 
   * @param word Raw word from user
   * @return Normalized word for dictionary lookup
   */
  static std::string cleanWord(const std::string& word);

 private:
  DictionaryStore() = default;

  struct IndexHit {
    std::string headword;
    uint32_t dictOffset = 0;
    uint32_t dictSize = 0;
  };

  static constexpr const char* DICTIONARY_ROOT = "/dictionaries";
  static constexpr const char* CONFIG_PATH = "/.crosspoint/dictionary_config.json";
  static constexpr const char* HISTORY_PATH = "/.crosspoint/dictionary_history.txt";
  static constexpr int CHECKPOINT_INTERVAL = 1024;
  static constexpr size_t MAX_CHECKPOINT_COUNT = 4096;
  static constexpr size_t MAX_SCAN_ENTRIES = 64;
  static constexpr size_t MAX_DEFINITION_BYTES = 8192;
  static constexpr size_t MIN_DEFINITION_BYTES = 1024;
  static constexpr size_t MAX_HISTORY_ITEMS = 15;

  std::vector<DictionaryEntry> entries;
  DictionaryEntry activeOnlyEntry;
  std::string activeIfoPath;
  int activeIndex = -1;
  bool configLoaded = false;
  bool scanned = false;
  bool activeOnlyLoaded = false;
  uint8_t definitionTextSize = DEF_TEXT_SMALL;

  bool loadEntryFromIfoPath(const std::string& ifoPath, DictionaryEntry& entry) const;
  bool ensureActiveEntryLoaded();
  void clearActiveOnlyEntry();
  DictionaryEntry* activeEntry();
  const DictionaryEntry* activeEntry() const;
  bool ensurePrepared(DictionaryEntry& entry, const std::function<void(int percent)>& onProgress = nullptr);
  bool loadCheckpointCache(DictionaryEntry& entry);
  bool saveCheckpointCache(const DictionaryEntry& entry) const;
  bool findIndexHit(const DictionaryEntry& entry, const std::string& word, IndexHit& hit) const;
  bool lookupSynonym(const DictionaryEntry& entry, const std::string& word, std::string& canonical) const;
  std::string headwordAtOrdinal(const DictionaryEntry& entry, uint32_t ordinal) const;
  std::string readDefinition(const DictionaryEntry& entry, const IndexHit& hit, bool& truncated) const;
  std::vector<std::string> findSuggestions(const DictionaryEntry& entry, const std::string& word, int maxResults) const;
  std::vector<std::string> getFallbackForms(const DictionaryEntry& entry, const std::string& word) const;
};

#define DICTIONARIES DictionaryStore::getInstance()
