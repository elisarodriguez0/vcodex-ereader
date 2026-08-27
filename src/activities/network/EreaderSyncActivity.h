#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "activities/Activity.h"

struct OpdsServer;

class EreaderSyncActivity final : public Activity {
 public:
  explicit EreaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("EreaderSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state == State::SYNCING; }
  uint8_t getUiTransitionRefreshWeight() const override { return UI_TRANSITION_REFRESH_WEIGHT_DENSE; }

 private:
  enum class State {
    CONNECTING,
    SYNCING,
    DONE,
    ERROR,
  };

  struct RemoteItem {
    std::string name;
    std::string etag;
    std::string downloadUrl;
    size_t size = 0;
  };

  struct VersionEntry {
    char kind = 'B';
    std::string name;
    std::string etag;
  };

  struct Counters {
    int added = 0;
    int updated = 0;
    int unchanged = 0;
    int failed = 0;
  };

  State state = State::CONNECTING;
  std::string statusMessage;
  std::string errorMessage;
  std::vector<VersionEntry> versions;
  Counters books;
  Counters wallpapers;
  bool daySynced = false;
  bool wifiConnectedOnEnter = false;
  bool connectedInActivity = false;
  bool networkMemoryReleased = false;
  bool cancelRequested = false;

  void connectAndSync();
  void onWifiSelectionComplete(bool connected);
  void performSync();
  bool syncDay();

  bool loadServer(OpdsServer& server) const;
  static std::string manifestUrlFromOpds(const std::string& opdsUrl);
  bool fetchManifest(const OpdsServer& server, std::vector<RemoteItem>& remoteBooks,
                     std::vector<RemoteItem>& remoteWallpapers);

  void loadVersions();
  bool saveVersions() const;
  const std::string* versionFor(char kind, const std::string& name) const;
  void setVersion(char kind, const std::string& name, const std::string& etag);

  bool syncBook(const OpdsServer& server, const RemoteItem& item);
  bool syncWallpaper(const OpdsServer& server, const RemoteItem& item);
  bool downloadFile(const OpdsServer& server, const RemoteItem& item, const std::string& destination);

  static bool isSafeFilename(const std::string& name);
  static bool endsWithCaseInsensitive(const std::string& value, const char* suffix);
  static bool validateFileSize(const std::string& path, size_t expectedSize);
  static bool replaceAtomically(const std::string& partPath, const std::string& finalPath);
  static std::string bookBucket(const std::string& fileName);
  static std::string wallpaperStem(const std::string& fileName);

  void prepareNetworkMemory();
  void restoreNetworkMemory();
  void turnWifiOff();
  void setState(State newState, std::string message);
  void setError(std::string message);
};
