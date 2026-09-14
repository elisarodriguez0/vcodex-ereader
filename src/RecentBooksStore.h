#pragma once
#include <string>
#include <vector>

/**
 * Metadata for a recently opened book.
 * 
 * Used to populate the "Recent Books" screen and provide quick access
 * to frequently read titles. Metadata includes file path, title, author,
 * and cover image location.
 */
struct RecentBook {
  std::string bookId;         /**< Optional unique identifier (e.g., ISBN or EPUB id) */
  std::string path;           /**< SD card file path to the EPUB/TXT file */
  std::string title;          /**< Display title */
  std::string author;         /**< Display author name */
  std::string coverBmpPath;   /**< Path to cached cover image (PNG/BMP) */

  /**
   * Equality comparison using bookId (preferred) or path (fallback).
   * Books are considered equal if:
   * - Both have valid bookIds and they match, OR
   * - Path strings match (bookId-less matching)
   */
  bool operator==(const RecentBook& other) const {
    return !bookId.empty() && !other.bookId.empty() ? bookId == other.bookId : path == other.path;
  }
};

class RecentBooksStore;
namespace JsonSettingsIO {
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);
}  // namespace JsonSettingsIO

/**
 * Singleton store for recently opened books.
 * 
 * Manages the "Recent Books" list displayed on the home screen.
 * Persists to recentbooks.json on the SD card. Features include:
 * - LRU (Least Recently Used) ordering
 * - Duplicate prevention (moves existing book to front)
 * - Stale file detection and automatic pruning
 * - Metadata updating (title, author, cover image)
 * - Binary format support for legacy data migration
 */
class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<RecentBook> recentBooks;

  friend bool JsonSettingsIO::saveRecentBooks(const RecentBooksStore&, const char*);
  friend bool JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);

 public:
  ~RecentBooksStore() = default;

  /**
   * Get the singleton instance of the recent books store.
   */
  static RecentBooksStore& getInstance() { return instance; }

  /**
   * Add a book to the recent list.
   * 
   * If the book already exists (by bookId or path), it is moved to the front.
   * This function does not persist changes to disk; caller must call saveToFile().
   * 
   * @param path Absolute path to the book file on SD card
   * @param title Display title for the book
   * @param author Author name
   * @param coverBmpPath Path to cached cover image
   * @param bookId Optional unique identifier (ISBN, EPUB id, etc.)
   */
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath, const std::string& bookId = "");

  /**
   * Update all metadata fields for a book in the recent list.
   * 
   * @param path Path to the book file
   * @param title New title
   * @param author New author
   * @param coverBmpPath New cover image path
   * @param bookId Optional unique identifier
   */
  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath, const std::string& bookId = "");

  /**
   * Move a book entry from one path to another.
   * Used when a file is moved/renamed on the SD card.
   * 
   * @param oldKey Original book path or ID to look up
   * @param newPath New absolute path for the book
   * @param title Optional new title
   * @param author Optional new author  
   * @param coverBmpPath Optional new cover path
   * @param bookId Optional new unique identifier
   * @return true if updated successfully, false if book not found
   */
  bool updateBookPath(const std::string& oldKey, const std::string& newPath, const std::string& title = "",
                      const std::string& author = "", const std::string& coverBmpPath = "",
                      const std::string& bookId = "");

  /**
   * Remove a book from the recent list.
   * 
   * @param key Book path or ID to remove
   * @return true if removed successfully, false if book not found
   */
  bool removeBook(const std::string& key);

  /**
   * Check if a book's backing file is missing from the SD card.
   * 
   * @param book Book metadata to check
   * @return true if the file at book.path does not exist
   */
  static bool isMissing(const RecentBook& book);

  /**
   * Remove all entries whose backing files no longer exist on the SD card.
   * 
   * Performs disk checks for all entries. Does not persist changes;
   * caller must decide whether to saveToFile().
   * 
   * @return true if any entries were removed, false otherwise
   */
  bool pruneMissing();

  /**
   * Get the list of recent books in LRU order (most recent first).
   */
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  /**
   * Get the number of books in the recent list.
   */
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  /**
   * Persist all recent books to recentbooks.json on the SD card.
   * 
   * @return true if save succeeded, false on I/O error
   */
  bool saveToFile() const;

  /**
   * Load recent books from recentbooks.json on the SD card.
   * Falls back to legacy binary format if JSON doesn't exist.
   * 
   * @return true if load succeeded, false on I/O error or corrupted data
   */
  bool loadFromFile();

  /**
   * Extract book metadata from a file on the SD card.
   * Used to populate RecentBook fields when adding a new entry.
   * 
   * @param path Path to the book file
   * @return RecentBook with extracted metadata
   */
  RecentBook getDataFromBook(std::string path) const;

 private:
  /* Find the index of a book by path or ID. Returns -1 if not found. */
  int findBookIndex(const std::string& path, const std::string& bookId) const;

  /* Normalize title/author formatting (trim whitespace, remove escapes). */
  void normalizeBook(RecentBook& book);
  void normalizeBooks();

  /* Load recent books from legacy binary recentbooks.bak format. */
  bool loadFromBinaryFile();
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
