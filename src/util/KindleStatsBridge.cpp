#include "KindleStatsBridge.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "OpdsServerStore.h"
#include "ReadingStatsStore.h"
#include "network/HttpDownloader.h"
#include "util/TimeUtils.h"

namespace {

constexpr const char* LOG_TAG = "KSTATS";
constexpr const char* SERVER_NAME = "Ereader Sync";
constexpr const char* REMOTE_SNAPSHOT_FILE = "/.crosspoint/kindle_stats_remote.json";
constexpr const char* BASELINE_FILE = "/.crosspoint/kindle_stats_sync.json";
constexpr const char* BASELINE_TEMP_FILE = "/.crosspoint/kindle_stats_sync.json.tmp";
constexpr size_t MAX_REMOTE_SNAPSHOT_BYTES = 512 * 1024;

struct BaselineRecord {
  std::string book;
  std::string date;
  uint64_t readingSeconds = 0;
  uint32_t sessions = 0;
};

bool startsWithUtf8(const std::string& value, const char* prefix) {
  const std::string p = prefix;
  return value.size() >= p.size() && value.compare(0, p.size(), p) == 0;
}

bool endsWithCaseInsensitive(const std::string& value, const char* suffix) {
  const std::string suffixString = suffix ? suffix : "";
  if (suffixString.size() > value.size()) {
    return false;
  }

  const size_t offset = value.size() - suffixString.size();
  for (size_t i = 0; i < suffixString.size(); ++i) {
    const unsigned char left = static_cast<unsigned char>(value[offset + i]);
    const unsigned char right = static_cast<unsigned char>(suffixString[i]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }

  return true;
}

std::string fileNameFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string asciiLower(std::string value) {
  for (char& c : value) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc >= 'A' && uc <= 'Z') {
      c = static_cast<char>(uc - 'A' + 'a');
    }
  }
  return value;
}

bool sameFilename(const std::string& left, const std::string& right) {
  return asciiLower(fileNameFromPath(left)) == asciiLower(fileNameFromPath(right));
}

bool sameMetadata(const ReadingBookStats& localBook, const std::string& title, const std::string& author) {
  if (title.empty() || localBook.title.empty() || localBook.title != title) {
    return false;
  }

  if (!author.empty() && !localBook.author.empty() && localBook.author != author) {
    return false;
  }

  return true;
}

bool isSafeBookFilename(const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name.find("..") != std::string::npos) {
    return false;
  }

  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
    return false;
  }

  return endsWithCaseInsensitive(name, ".epub");
}

std::string bookBucket(const std::string& fileName) {
  for (size_t i = 0; i < fileName.size();) {
    const unsigned char c = static_cast<unsigned char>(fileName[i]);

    if (c < 0x80) {
      if (std::isdigit(c)) return "0-9";
      if (std::isalpha(c)) return std::string(1, static_cast<char>(std::toupper(c)));
      ++i;
      continue;
    }

    const std::string rest = fileName.substr(i);

    if (startsWithUtf8(rest, "Á") || startsWithUtf8(rest, "À") || startsWithUtf8(rest, "Â") ||
        startsWithUtf8(rest, "Ä") || startsWithUtf8(rest, "Ã") || startsWithUtf8(rest, "á") ||
        startsWithUtf8(rest, "à") || startsWithUtf8(rest, "â") || startsWithUtf8(rest, "ä") ||
        startsWithUtf8(rest, "ã")) return "A";

    if (startsWithUtf8(rest, "É") || startsWithUtf8(rest, "È") || startsWithUtf8(rest, "Ê") ||
        startsWithUtf8(rest, "Ë") || startsWithUtf8(rest, "é") || startsWithUtf8(rest, "è") ||
        startsWithUtf8(rest, "ê") || startsWithUtf8(rest, "ë")) return "E";

    if (startsWithUtf8(rest, "Í") || startsWithUtf8(rest, "Ì") || startsWithUtf8(rest, "Î") ||
        startsWithUtf8(rest, "Ï") || startsWithUtf8(rest, "í") || startsWithUtf8(rest, "ì") ||
        startsWithUtf8(rest, "î") || startsWithUtf8(rest, "ï")) return "I";

    if (startsWithUtf8(rest, "Ó") || startsWithUtf8(rest, "Ò") || startsWithUtf8(rest, "Ô") ||
        startsWithUtf8(rest, "Ö") || startsWithUtf8(rest, "Õ") || startsWithUtf8(rest, "ó") ||
        startsWithUtf8(rest, "ò") || startsWithUtf8(rest, "ô") || startsWithUtf8(rest, "ö") ||
        startsWithUtf8(rest, "õ")) return "O";

    if (startsWithUtf8(rest, "Ú") || startsWithUtf8(rest, "Ù") || startsWithUtf8(rest, "Û") ||
        startsWithUtf8(rest, "Ü") || startsWithUtf8(rest, "ú") || startsWithUtf8(rest, "ù") ||
        startsWithUtf8(rest, "û") || startsWithUtf8(rest, "ü")) return "U";

    if (startsWithUtf8(rest, "Ñ") || startsWithUtf8(rest, "ñ")) return "N";
    if (startsWithUtf8(rest, "Ç") || startsWithUtf8(rest, "ç")) return "C";

    if ((c & 0xE0) == 0xC0) {
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      i += 4;
    } else {
      ++i;
    }
  }

  return "#";
}

std::string resolveLocalBookPath(const std::string& book, const std::string& title, const std::string& author) {
  if (!isSafeBookFilename(book)) {
    return {};
  }

  // Normal location used by Ereader Sync.
  const std::string bucketPath = std::string("/Books/") + bookBucket(book) + "/" + book;
  if (Storage.exists(bucketPath.c_str())) {
    return bucketPath;
  }

  // VCodex can move completed books here.
  const std::string finishedPath = std::string("/finished_books/") + book;
  if (Storage.exists(finishedPath.c_str())) {
    return finishedPath;
  }

  // Existing ReadingStatsStore paths are the most reliable source if a book
  // has been moved or reorganized after it was downloaded.
  for (const auto& localBook : READING_STATS.getBooks()) {
    if (localBook.path.empty() || !Storage.exists(localBook.path.c_str())) {
      continue;
    }

    if (sameFilename(localBook.path, book)) {
      return localBook.path;
    }

    for (const auto& knownPath : localBook.knownPaths) {
      if (!knownPath.empty() && Storage.exists(knownPath.c_str()) && sameFilename(knownPath, book)) {
        return knownPath;
      }
    }
  }

  // Metadata fallback for the same title/author with a path/name that changed.
  std::string matchedPath;
  for (const auto& localBook : READING_STATS.getBooks()) {
    if (localBook.path.empty() || !Storage.exists(localBook.path.c_str()) ||
        !sameMetadata(localBook, title, author)) {
      continue;
    }

    if (!matchedPath.empty()) {
      // Ambiguous metadata match: do not guess.
      return {};
    }

    matchedPath = localBook.path;
  }

  return matchedPath;
}

const OpdsServer* findEreaderServer() {
  const auto& servers = OPDS_STORE.getServers();

  for (const auto& server : servers) {
    if (server.name == SERVER_NAME && !server.url.empty()) {
      return &server;
    }
  }

  if (servers.size() == 1 && !servers.front().url.empty()) {
    return &servers.front();
  }

  return nullptr;
}

std::string statsUrlFromOpds(const std::string& opdsUrl) {
  if (opdsUrl.empty()) {
    return {};
  }

  const size_t queryPos = opdsUrl.find('?');
  std::string base = queryPos == std::string::npos ? opdsUrl : opdsUrl.substr(0, queryPos);
  const std::string query = queryPos == std::string::npos ? std::string{} : opdsUrl.substr(queryPos);

  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }

  static constexpr const char* OPDS_SUFFIX = "/opds";
  static constexpr size_t OPDS_SUFFIX_LENGTH = 5;

  if (base.size() < OPDS_SUFFIX_LENGTH ||
      base.compare(base.size() - OPDS_SUFFIX_LENGTH, OPDS_SUFFIX_LENGTH, OPDS_SUFFIX) != 0) {
    return {};
  }

  base.erase(base.size() - OPDS_SUFFIX_LENGTH);
  return base + "/stats/kindle" + query;
}

uint32_t parseDayOrdinal(const char* date) {
  if (!date || std::strlen(date) != 10) {
    return 0;
  }

  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  int consumed = 0;

  if (std::sscanf(date, "%4d-%2u-%2u%n", &year, &month, &day, &consumed) != 3 || consumed != 10) {
    return 0;
  }

  return TimeUtils::getDayOrdinalForDate(year, month, day);
}

BaselineRecord* findBaseline(std::vector<BaselineRecord>& baseline, const std::string& book,
                             const std::string& date) {
  for (auto& record : baseline) {
    if (record.book == book && record.date == date) {
      return &record;
    }
  }

  return nullptr;
}

bool loadBaseline(std::vector<BaselineRecord>& baseline) {
  baseline.clear();

  if (!Storage.exists(BASELINE_FILE)) {
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, BASELINE_FILE, file)) {
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file);
  file.close();

  if (error) {
    LOG_ERR(LOG_TAG, "Baseline JSON parse failed: %s", error.c_str());
    return false;
  }

  const JsonArrayConst records = document["records"].as<JsonArrayConst>();
  if (records.isNull()) {
    return true;
  }

  baseline.reserve(records.size());

  for (const JsonObjectConst object : records) {
    const char* book = object["book"] | "";
    const char* date = object["date"] | "";

    if (!book[0] || !date[0]) {
      continue;
    }

    BaselineRecord record;
    record.book = book;
    record.date = date;
    record.readingSeconds = object["reading_seconds"].as<uint64_t>();
    record.sessions = object["sessions"].as<uint32_t>();
    baseline.push_back(std::move(record));
  }

  return true;
}

bool saveBaseline(const std::vector<BaselineRecord>& baseline) {
  Storage.mkdir("/.crosspoint");

  if (Storage.exists(BASELINE_TEMP_FILE)) {
    Storage.remove(BASELINE_TEMP_FILE);
  }

  FsFile file;
  if (!Storage.openFileForWrite(LOG_TAG, BASELINE_TEMP_FILE, file)) {
    return false;
  }

  JsonDocument document;
  document["schema_version"] = 1;
  JsonArray records = document["records"].to<JsonArray>();

  for (const auto& record : baseline) {
    JsonObject object = records.add<JsonObject>();
    object["book"] = record.book;
    object["date"] = record.date;
    object["reading_seconds"] = record.readingSeconds;
    object["sessions"] = record.sessions;
  }

  const size_t written = serializeJson(document, file);
  file.flush();
  file.close();

  if (written == 0) {
    Storage.remove(BASELINE_TEMP_FILE);
    return false;
  }

  if (Storage.exists(BASELINE_FILE) && !Storage.remove(BASELINE_FILE)) {
    Storage.remove(BASELINE_TEMP_FILE);
    return false;
  }

  if (!Storage.rename(BASELINE_TEMP_FILE, BASELINE_FILE)) {
    Storage.remove(BASELINE_TEMP_FILE);
    return false;
  }

  return true;
}

}  // namespace

bool KindleStatsBridge::downloadSnapshot() {
  const OpdsServer* server = findEreaderServer();
  if (!server) {
    LOG_ERR(LOG_TAG, "No Ereader Sync OPDS server configured");
    return false;
  }

  const std::string url = statsUrlFromOpds(server->url);
  if (url.empty()) {
    LOG_ERR(LOG_TAG, "Ereader Sync OPDS URL does not end in /opds");
    return false;
  }

  Storage.mkdir("/.crosspoint");

  const auto result =
      HttpDownloader::downloadToFile(url, REMOTE_SNAPSHOT_FILE, nullptr, nullptr, server->username, server->password);

  if (result != HttpDownloader::OK) {
    LOG_ERR(LOG_TAG, "Could not download Kindle stats snapshot");
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, REMOTE_SNAPSHOT_FILE, file)) {
    return false;
  }

  const size_t size = file.size();
  file.close();

  if (size == 0 || size > MAX_REMOTE_SNAPSHOT_BYTES) {
    LOG_ERR(LOG_TAG, "Kindle stats snapshot has invalid size: %zu", size);
    Storage.remove(REMOTE_SNAPSHOT_FILE);
    return false;
  }

  LOG_INF(LOG_TAG, "Kindle stats snapshot downloaded: %zu bytes", size);
  return true;
}

bool KindleStatsBridge::importDownloadedSnapshot() {
  if (!Storage.exists(REMOTE_SNAPSHOT_FILE)) {
    LOG_ERR(LOG_TAG, "No staged Kindle stats snapshot found");
    return false;
  }

  FsFile remoteFile;
  if (!Storage.openFileForRead(LOG_TAG, REMOTE_SNAPSHOT_FILE, remoteFile)) {
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, remoteFile);
  remoteFile.close();

  if (error) {
    LOG_ERR(LOG_TAG, "Kindle snapshot JSON parse failed: %s", error.c_str());
    return false;
  }

  if ((document["schema_version"] | 0) != 1) {
    LOG_ERR(LOG_TAG, "Unsupported Kindle stats schema");
    return false;
  }

  const char* device = document["device"] | "";
  if (std::string(device) != "kindle") {
    LOG_ERR(LOG_TAG, "Unexpected Kindle stats device value");
    return false;
  }

  const JsonArrayConst days = document["days"].as<JsonArrayConst>();
  if (days.isNull()) {
    LOG_ERR(LOG_TAG, "Kindle stats snapshot has no days array");
    return false;
  }

  std::vector<BaselineRecord> baseline;
  if (!loadBaseline(baseline)) {
    LOG_ERR(LOG_TAG, "Could not load Kindle stats baseline");
    return false;
  }

  uint32_t importedRecords = 0;
  uint64_t importedMs = 0;
  uint32_t importedSessions = 0;
  uint32_t missingBooks = 0;
  uint32_t unchangedRecords = 0;
  bool statsChanged = false;
  bool baselineChanged = false;

  for (const JsonObjectConst object : days) {
    const char* bookText = object["book"] | "";
    const char* titleText = object["title"] | "";
    const char* authorText = object["author"] | "";
    const char* dateText = object["date"] | "";

    const std::string book = bookText;
    const std::string title = titleText;
    const std::string author = authorText;
    const std::string date = dateText;

    if (!isSafeBookFilename(book) || date.empty()) {
      continue;
    }

    const uint32_t dayOrdinal = parseDayOrdinal(dateText);
    if (dayOrdinal == 0) {
      continue;
    }

    const uint64_t remoteSeconds = object["reading_seconds"].as<uint64_t>();
    const uint32_t remoteSessions = object["sessions"].as<uint32_t>();

    const std::string path = resolveLocalBookPath(book, title, author);
    if (path.empty()) {
      ++missingBooks;
      LOG_DBG(LOG_TAG, "No local match for Kindle book: %s", book.c_str());
      continue;
    }

    BaselineRecord* previous = findBaseline(baseline, book, date);
    if (!previous) {
      baseline.push_back(BaselineRecord{book, date, 0, 0});
      previous = &baseline.back();
    }

    const bool remoteReset =
        remoteSeconds < previous->readingSeconds || remoteSessions < previous->sessions;

    const uint64_t deltaSeconds =
        remoteReset ? 0 : remoteSeconds - previous->readingSeconds;
    const uint32_t deltaSessions =
        remoteReset ? 0 : remoteSessions - previous->sessions;

    if (deltaSeconds > 0 || deltaSessions > 0) {
      const uint64_t deltaMs = deltaSeconds * 1000ULL;

      if (!READING_STATS.importExternalReadingStats(path, title, author, dayOrdinal, deltaMs, deltaSessions)) {
        LOG_ERR(LOG_TAG, "Could not import Kindle stats for %s on %s", book.c_str(), date.c_str());
        continue;
      }

      ++importedRecords;
      importedMs += deltaMs;
      importedSessions += deltaSessions;
      statsChanged = true;
    } else {
      ++unchangedRecords;
    }

    if (previous->readingSeconds != remoteSeconds || previous->sessions != remoteSessions) {
      previous->readingSeconds = remoteSeconds;
      previous->sessions = remoteSessions;
      baselineChanged = true;
    }
  }

  if (statsChanged && !READING_STATS.saveToFile()) {
    LOG_ERR(LOG_TAG, "Could not persist imported Kindle reading stats");
    return false;
  }

  if (baselineChanged && !saveBaseline(baseline)) {
    LOG_ERR(LOG_TAG, "Could not persist Kindle stats baseline");
    return false;
  }

  LOG_INF(LOG_TAG,
          "Kindle stats pull: imported_records=%u time_ms=%llu sessions=%u missing_books=%u unchanged=%u",
          importedRecords, static_cast<unsigned long long>(importedMs), importedSessions, missingBooks,
          unchangedRecords);

  return true;
}
