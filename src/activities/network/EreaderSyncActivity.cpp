#include "EreaderSyncActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ReadingStatsStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/NetworkMemory.h"
#include "util/TimeUtils.h"

namespace {
constexpr const char* LOG_TAG = "ESYNC";
constexpr const char* SERVER_NAME = "Ereader Sync";
constexpr const char* BOOKS_DIR = "/Books";
constexpr const char* SLEEP_DIR = "/.sleep";
constexpr const char* CROSSPOINT_DIR = "/.crosspoint";
constexpr const char* VERSION_FILE = "/.crosspoint/ereader_sync.json";
constexpr const char* WALLPAPER_TEMP_JPG = "/.crosspoint/ereader_wallpaper.jpg.part";
constexpr size_t MAX_MANIFEST_BYTES = 256 * 1024;

bool startsWithUtf8(const std::string& value, const char* prefix) {
  const std::string p = prefix;
  return value.size() >= p.size() && value.compare(0, p.size(), p) == 0;
}

std::string extensionLower(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = name.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}
}  // namespace

void EreaderSyncActivity::onEnter() {
  Activity::onEnter();

  books = {};
  wallpapers = {};
  versions.clear();
  statusMessage.clear();
  errorMessage.clear();
  daySynced = false;
  connectedInActivity = false;
  networkMemoryReleased = false;
  cancelRequested = false;
  wifiConnectedOnEnter = WiFi.status() == WL_CONNECTED;

#ifdef SIMULATOR
  setError("Sync all is only available on the device");
#else
  connectAndSync();
#endif
}

void EreaderSyncActivity::onExit() {
  restoreNetworkMemory();
  if (!wifiConnectedOnEnter && connectedInActivity) {
    turnWifiOff();
  }
  Activity::onExit();
}

void EreaderSyncActivity::loop() {
  if (state == State::SYNCING || state == State::CONNECTING) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void EreaderSyncActivity::connectAndSync() {
  TimeUtils::configureTimezone();

  if (WiFi.status() == WL_CONNECTED) {
    performSync();
    return;
  }

  WiFi.mode(WIFI_STA);
  setState(State::CONNECTING, "Connecting to Wi-Fi...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, false),
                         [this](const ActivityResult& result) {
                           onWifiSelectionComplete(!result.isCancelled && WiFi.status() == WL_CONNECTED);
                         });
}

void EreaderSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    setError("Wi-Fi connection cancelled");
    return;
  }

  if (!wifiConnectedOnEnter) connectedInActivity = true;
  performSync();
}

bool EreaderSyncActivity::syncDay() {
  setState(State::SYNCING, "Syncing day...");
  requestUpdateAndWait();

  const bool hadValidTimeBefore = TimeUtils::isClockValid();
  const bool ntpSuccess = TimeUtils::syncTimeWithNtp();
  const uint32_t currentValidTimestamp = TimeUtils::getCurrentValidTimestamp();
  const bool effectiveSuccess = ntpSuccess || (!hadValidTimeBefore && currentValidTimestamp > 0);

  if (effectiveSuccess && currentValidTimestamp > 0) {
    APP_STATE.registerValidTimeSync(currentValidTimestamp);
    APP_STATE.saveToFile();
    if (READING_STATS.isAutoBackupDue()) {
      READING_STATS.createDueAutoBackup();
    }
  }

  return effectiveSuccess;
}

void EreaderSyncActivity::prepareNetworkMemory() {
  if (networkMemoryReleased) return;
  NetworkMemory::prepareBeforeNetwork(renderer, LOG_TAG, "ereader-sync", true);
  networkMemoryReleased = true;
}

void EreaderSyncActivity::restoreNetworkMemory() {
  if (!networkMemoryReleased) return;
  NetworkMemory::restoreAfterNetwork(renderer, LOG_TAG, "ereader-sync", true);
  networkMemoryReleased = false;
}

void EreaderSyncActivity::turnWifiOff() {
  TimeUtils::stopNtp();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

bool EreaderSyncActivity::loadServer(OpdsServer& server) const {
  const auto& servers = OPDS_STORE.getServers();
  const OpdsServer* selected = nullptr;

  for (const auto& candidate : servers) {
    if (candidate.name == SERVER_NAME) {
      selected = &candidate;
      break;
    }
  }

  if (!selected && servers.size() == 1) selected = &servers.front();
  if (!selected || selected->url.empty()) return false;

  server = *selected;
  return true;
}

std::string EreaderSyncActivity::manifestUrlFromOpds(const std::string& opdsUrl) {
  if (opdsUrl.empty()) return {};

  const size_t queryPosition = opdsUrl.find('?');
  std::string base = queryPosition == std::string::npos ? opdsUrl : opdsUrl.substr(0, queryPosition);
  const std::string query = queryPosition == std::string::npos ? std::string{} : opdsUrl.substr(queryPosition);

  while (!base.empty() && base.back() == '/') base.pop_back();

  constexpr const char* OPDS_SUFFIX = "/opds";
  constexpr size_t OPDS_SUFFIX_LENGTH = 5;
  if (base.size() < OPDS_SUFFIX_LENGTH ||
      base.compare(base.size() - OPDS_SUFFIX_LENGTH, OPDS_SUFFIX_LENGTH, OPDS_SUFFIX) != 0) {
    return {};
  }

  base.resize(base.size() - OPDS_SUFFIX_LENGTH);
  return base + "/manifest" + query;
}

bool EreaderSyncActivity::fetchManifest(const OpdsServer& server, std::vector<RemoteItem>& remoteBooks,
                                        std::vector<RemoteItem>& remoteWallpapers) {
  const std::string manifestUrl = manifestUrlFromOpds(server.url);
  if (manifestUrl.empty()) {
    setError("Ereader Sync OPDS URL must end in /opds");
    return false;
  }

  setState(State::SYNCING, "Fetching manifest...");
  requestUpdateAndWait();

  std::string responseBody;
  if (!HttpDownloader::fetchUrl(manifestUrl, responseBody, server.username, server.password)) {
    setError("Could not download manifest");
    return false;
  }

  if (responseBody.size() > MAX_MANIFEST_BYTES) {
    setError("Manifest is too large");
    return false;
  }

  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, responseBody);
  responseBody.clear();
  responseBody.shrink_to_fit();

  if (parseError) {
    LOG_ERR(LOG_TAG, "Manifest JSON error: %s", parseError.c_str());
    setError("Could not parse manifest");
    return false;
  }

  const auto parseArray = [](const JsonVariantConst source, std::vector<RemoteItem>& destination) {
    const JsonArrayConst array = source.as<JsonArrayConst>();
    if (array.isNull()) return;
    destination.reserve(array.size());

    for (const JsonObjectConst object : array) {
      const char* name = object["name"] | "";
      const char* etag = object["etag"] | "";
      const char* downloadUrl = object["download_url"] | "";
      const size_t size = object["size"] | 0;
      if (!name[0] || !downloadUrl[0]) continue;

      RemoteItem item;
      item.name = name;
      item.etag = etag;
      item.downloadUrl = downloadUrl;
      item.size = size;
      destination.push_back(std::move(item));
    }
  };

  parseArray(document["books"], remoteBooks);
  parseArray(document["wallpapers_xteink"], remoteWallpapers);
  return true;
}

void EreaderSyncActivity::loadVersions() {
  versions.clear();
  if (!Storage.exists(VERSION_FILE)) return;

  const String raw = Storage.readFile(VERSION_FILE);
  if (raw.length() == 0) return;

  JsonDocument document;
  if (deserializeJson(document, raw)) {
    LOG_ERR(LOG_TAG, "Ignoring invalid sync state");
    return;
  }

  const auto loadSection = [this, &document](const char* section, const char kind) {
    const JsonObjectConst object = document[section].as<JsonObjectConst>();
    if (object.isNull()) return;
    for (const JsonPairConst pair : object) {
      const char* etag = pair.value().as<const char*>();
      if (!etag) continue;
      versions.push_back(VersionEntry{kind, pair.key().c_str(), etag});
    }
  };

  loadSection("books", 'B');
  loadSection("wallpapers", 'W');
}

bool EreaderSyncActivity::saveVersions() const {
  if (!Storage.ensureDirectoryExists(CROSSPOINT_DIR)) return false;

  JsonDocument document;
  for (const auto& entry : versions) {
    document[entry.kind == 'W' ? "wallpapers" : "books"][entry.name.c_str()] = entry.etag;
  }

  String serialized;
  if (serializeJson(document, serialized) == 0) return false;
  return Storage.writeFile(VERSION_FILE, serialized);
}

const std::string* EreaderSyncActivity::versionFor(const char kind, const std::string& name) const {
  for (const auto& entry : versions) {
    if (entry.kind == kind && entry.name == name) return &entry.etag;
  }
  return nullptr;
}

void EreaderSyncActivity::setVersion(const char kind, const std::string& name, const std::string& etag) {
  for (auto& entry : versions) {
    if (entry.kind == kind && entry.name == name) {
      entry.etag = etag;
      return;
    }
  }
  versions.push_back(VersionEntry{kind, name, etag});
}

bool EreaderSyncActivity::isSafeFilename(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  for (const unsigned char character : name) {
    if (character < 0x20 || character == '/' || character == '\\') return false;
  }
  return true;
}

bool EreaderSyncActivity::endsWithCaseInsensitive(const std::string& value, const char* suffix) {
  if (!suffix) return false;
  const std::string ending = suffix;
  if (value.size() < ending.size()) return false;

  const size_t offset = value.size() - ending.size();
  for (size_t i = 0; i < ending.size(); ++i) {
    const unsigned char left = static_cast<unsigned char>(value[offset + i]);
    const unsigned char right = static_cast<unsigned char>(ending[i]);
    if (std::tolower(left) != std::tolower(right)) return false;
  }
  return true;
}

bool EreaderSyncActivity::validateFileSize(const std::string& path, const size_t expectedSize) {
  if (expectedSize == 0) return true;

  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  const size_t actualSize = file.fileSize();
  file.close();
  return actualSize == expectedSize;
}

bool EreaderSyncActivity::replaceAtomically(const std::string& partPath, const std::string& finalPath) {
  const std::string backupPath = finalPath + ".bak";
  if (Storage.exists(backupPath.c_str())) Storage.remove(backupPath.c_str());

  const bool hadExistingFile = Storage.exists(finalPath.c_str());
  if (hadExistingFile && !Storage.rename(finalPath.c_str(), backupPath.c_str())) return false;

  if (!Storage.rename(partPath.c_str(), finalPath.c_str())) {
    if (hadExistingFile) Storage.rename(backupPath.c_str(), finalPath.c_str());
    return false;
  }

  if (hadExistingFile) Storage.remove(backupPath.c_str());
  return true;
}

std::string EreaderSyncActivity::bookBucket(const std::string& fileName) {
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

    if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else ++i;
  }
  return "#";
}

std::string EreaderSyncActivity::wallpaperStem(const std::string& fileName) {
  const size_t dot = fileName.find_last_of('.');
  return dot == std::string::npos ? fileName : fileName.substr(0, dot);
}

bool EreaderSyncActivity::downloadFile(const OpdsServer& server, const RemoteItem& item,
                                       const std::string& destination) {
  if (Storage.exists(destination.c_str())) Storage.remove(destination.c_str());

  cancelRequested = false;
  auto pollProgress = [this](const size_t, const size_t) {
    mappedInput.update();
    if (mappedInput.isPressed(MappedInputManager::Button::Back)) cancelRequested = true;
  };

  const auto result = HttpDownloader::downloadToFile(item.downloadUrl, destination, pollProgress, &cancelRequested,
                                                      server.username, server.password);
  if (result != HttpDownloader::OK) {
    if (Storage.exists(destination.c_str())) Storage.remove(destination.c_str());
    return false;
  }

  if (!validateFileSize(destination, item.size)) {
    Storage.remove(destination.c_str());
    return false;
  }

  return true;
}

bool EreaderSyncActivity::syncBook(const OpdsServer& server, const RemoteItem& item) {
  if (!isSafeFilename(item.name) || !endsWithCaseInsensitive(item.name, ".epub")) {
    books.failed++;
    return false;
  }

  const std::string bucket = bookBucket(item.name);
  const std::string directory = std::string(BOOKS_DIR) + "/" + bucket;
  if (!Storage.ensureDirectoryExists(directory.c_str())) {
    books.failed++;
    return false;
  }

  const std::string finalPath = directory + "/" + item.name;
  const std::string* storedVersion = versionFor('B', item.name);

  if (!item.etag.empty() && storedVersion && *storedVersion == item.etag && Storage.exists(finalPath.c_str())) {
    books.unchanged++;
    return true;
  }

  const bool existed = Storage.exists(finalPath.c_str());
  const std::string partPath = finalPath + ".part";
  if (!downloadFile(server, item, partPath)) {
    if (!cancelRequested) books.failed++;
    return false;
  }

  if (!replaceAtomically(partPath, finalPath)) {
    Storage.remove(partPath.c_str());
    books.failed++;
    return false;
  }

  clearBookCache(finalPath);
  existed ? books.updated++ : books.added++;
  setVersion('B', item.name, item.etag);
  return true;
}

bool EreaderSyncActivity::syncWallpaper(const OpdsServer& server, const RemoteItem& item) {
  if (!isSafeFilename(item.name)) {
    wallpapers.failed++;
    return false;
  }

  const std::string ext = extensionLower(item.name);
  const bool direct = ext == ".png" || ext == ".bmp";
  const bool jpeg = ext == ".jpg" || ext == ".jpeg";
  if (!direct && !jpeg) {
    wallpapers.failed++;
    return false;
  }

  const std::string stem = wallpaperStem(item.name);
  const std::string finalName = direct ? item.name : stem + ".bmp";
  const std::string finalPath = std::string(SLEEP_DIR) + "/" + finalName;
  const std::string effectiveVersion = jpeg ? "photo-fs-v1:" + item.etag : item.etag;
  const std::string* storedVersion = versionFor('W', item.name);

  if (!item.etag.empty() && storedVersion && *storedVersion == effectiveVersion && Storage.exists(finalPath.c_str())) {
    wallpapers.unchanged++;
    return true;
  }

  const bool existed = Storage.exists(finalPath.c_str());

  if (direct) {
    const std::string partPath = finalPath + ".part";
    if (!downloadFile(server, item, partPath)) {
      if (!cancelRequested) wallpapers.failed++;
      return false;
    }
    if (!replaceAtomically(partPath, finalPath)) {
      Storage.remove(partPath.c_str());
      wallpapers.failed++;
      return false;
    }

    // When the backend upgrades a wallpaper from old JPG/BMP output to PNG,
    // remove the obsolete BMP with the same stem so shuffle does not show both.
    if (ext == ".png") {
      const std::string legacyBmp = std::string(SLEEP_DIR) + "/" + stem + ".bmp";
      if (legacyBmp != finalPath && Storage.exists(legacyBmp.c_str())) Storage.remove(legacyBmp.c_str());
    }
  } else {
    if (!downloadFile(server, item, WALLPAPER_TEMP_JPG)) {
      if (!cancelRequested) wallpapers.failed++;
      return false;
    }

    const std::string bmpPart = finalPath + ".part";
    if (Storage.exists(bmpPart.c_str())) Storage.remove(bmpPart.c_str());

    FsFile jpegFile;
    FsFile bmpFile;
    bool converted = false;
    if (Storage.openFileForRead(LOG_TAG, WALLPAPER_TEMP_JPG, jpegFile) &&
        Storage.openFileForWrite(LOG_TAG, bmpPart, bmpFile)) {
      converted = JpegToBmpConverter::jpegFileToPhotoBmpStream(jpegFile, bmpFile, true);
      if (converted) {
        bmpFile.flush();
      }
    }
    jpegFile.close();
    bmpFile.close();
    Storage.remove(WALLPAPER_TEMP_JPG);

    if (!converted || !replaceAtomically(bmpPart, finalPath)) {
      Storage.remove(bmpPart.c_str());
      wallpapers.failed++;
      return false;
    }
  }

  existed ? wallpapers.updated++ : wallpapers.added++;
  setVersion('W', item.name, effectiveVersion);
  return true;
}

void EreaderSyncActivity::performSync() {
  OpdsServer server;
  if (!loadServer(server)) {
    setError("Configure an OPDS server named Ereader Sync");
    return;
  }

  daySynced = syncDay();
  prepareNetworkMemory();

  std::vector<RemoteItem> remoteBooks;
  std::vector<RemoteItem> remoteWallpapers;
  if (!fetchManifest(server, remoteBooks, remoteWallpapers)) {
    restoreNetworkMemory();
    return;
  }

  if (!Storage.ensureDirectoryExists(BOOKS_DIR) || !Storage.ensureDirectoryExists(SLEEP_DIR) ||
      !Storage.ensureDirectoryExists(CROSSPOINT_DIR)) {
    restoreNetworkMemory();
    setError("Could not create sync folders");
    return;
  }

  loadVersions();

  setState(State::SYNCING, "Syncing books...");
  requestUpdateAndWait();
  for (const auto& item : remoteBooks) {
    if (cancelRequested) break;
    syncBook(server, item);
  }

  if (!cancelRequested) {
    setState(State::SYNCING, "Syncing wallpapers...");
    requestUpdateAndWait();
    for (const auto& item : remoteWallpapers) {
      if (cancelRequested) break;
      syncWallpaper(server, item);
    }
  }

  const bool stateSaved = saveVersions();
  restoreNetworkMemory();

  if (cancelRequested) {
    setError("Sync cancelled");
    return;
  }
  if (!stateSaved) {
    setError("Files synced but sync state could not be saved");
    return;
  }
  if (books.failed > 0 || wallpapers.failed > 0) {
    setError("Sync finished with errors");
    return;
  }

  setState(State::DONE, "Everything is up to date");
}

void EreaderSyncActivity::setState(const State newState, std::string message) {
  state = newState;
  statusMessage = std::move(message);
  requestUpdate();
}

void EreaderSyncActivity::setError(std::string message) {
  LOG_ERR(LOG_TAG, "%s", message.c_str());
  errorMessage = std::move(message);
  state = State::ERROR;
  requestUpdate();
}

void EreaderSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Sync all");

  const int centerY = pageHeight / 2 - lineHeight * 2;
  if (state == State::DONE || state == State::ERROR) {
    const bool success = state == State::DONE;
    renderer.drawCenteredText(UI_12_FONT_ID, centerY, success ? "Sync complete" : "Sync error", true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight + metrics.verticalSpacing,
                              success ? statusMessage.c_str() : errorMessage.c_str(), true);

    char dayLine[48];
    char booksLine[96];
    char wallpapersLine[96];
    snprintf(dayLine, sizeof(dayLine), "Day: %s", daySynced ? "synced" : "not synced");
    snprintf(booksLine, sizeof(booksLine), "Books: +%d  updated %d  unchanged %d  failed %d", books.added,
             books.updated, books.unchanged, books.failed);
    snprintf(wallpapersLine, sizeof(wallpapersLine), "Wallpapers: +%d  updated %d  unchanged %d  failed %d",
             wallpapers.added, wallpapers.updated, wallpapers.unchanged, wallpapers.failed);

    renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight * 2 + metrics.verticalSpacing * 2, dayLine, true);
    renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight * 3 + metrics.verticalSpacing * 2, booksLine, true);
    renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight * 4 + metrics.verticalSpacing * 2, wallpapersLine,
                              true);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, centerY, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight + metrics.verticalSpacing,
                              "Back cancels after the current network chunk", true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), state == State::DONE || state == State::ERROR ? tr(STR_OK_BUTTON) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}