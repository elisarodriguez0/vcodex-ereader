#include "SilentTimeSync.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdint>
#include <optional>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "WifiCredentialStore.h"
#include "util/TimeUtils.h"

namespace {

constexpr uint32_t POST_BOOT_DELAY_MS = 2000;
constexpr uint32_t USER_ACTIVITY_RETRY_DELAY_MS = 3000;
constexpr uint32_t SCAN_TIMEOUT_MS = 4000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 6000;
constexpr uint32_t NTP_TIMEOUT_MS = 4000;

enum class State : uint8_t {
  IDLE,
  WAITING,
  SCANNING,
  CONNECTING,
  NTP,
};

struct Candidate {
  WifiCredential credential;
  int32_t rssi = INT32_MIN;
  int32_t channel = 0;
  uint8_t bssid[6] = {};
  bool valid = false;
  bool hasBssid = false;
  bool preferred = false;
};

State state = State::IDLE;
uint32_t deadlineMs = 0;
Candidate candidate;

bool deadlineReached(const uint32_t now, const uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void shutDownWifi() {
  TimeUtils::stopNtp();
  WiFi.scanDelete();
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

void finishAttempt() {
  shutDownWifi();
  candidate = Candidate{};
  deadlineMs = 0;
  state = State::IDLE;
}

void postponeAfterUserActivity() {
  shutDownWifi();
  candidate = Candidate{};
  deadlineMs = millis() + USER_ACTIVITY_RETRY_DELAY_MS;
  state = State::WAITING;
}

bool selectCandidate(Candidate& selected) {
  const int16_t count = WiFi.scanComplete();
  if (count <= 0) {
    return false;
  }

  const std::string preferredSsid = WIFI_STORE.getLastConnectedSsid();

  for (int16_t i = 0; i < count; ++i) {
    const String scannedSsid = WiFi.SSID(i);
    if (scannedSsid.isEmpty()) {
      continue;
    }

    const std::optional<WifiCredential> credential =
        WIFI_STORE.findCredential(std::string(scannedSsid.c_str()));
    if (!credential.has_value()) {
      continue;
    }

    const bool isPreferred =
        !preferredSsid.empty() && credential->ssid == preferredSsid;
    const int32_t rssi = WiFi.RSSI(i);

    if (selected.valid) {
      if (selected.preferred && !isPreferred) {
        continue;
      }
      if (selected.preferred == isPreferred && rssi <= selected.rssi) {
        continue;
      }
    }

    selected.credential = *credential;
    selected.rssi = rssi;
    selected.channel = WiFi.channel(i);
    selected.hasBssid =
        selected.channel > 0 && WiFi.BSSID(i, selected.bssid);
    selected.preferred = isPreferred;
    selected.valid = true;
  }

  return selected.valid;
}

bool alreadySynchronizedForCurrentDay() {
  const uint32_t currentTimestamp = TimeUtils::getCurrentValidTimestamp();
  if (!TimeUtils::isClockValid(currentTimestamp) ||
      !TimeUtils::isClockValid(APP_STATE.lastKnownValidTimestamp)) {
    return false;
  }

  const uint32_t currentDay =
      TimeUtils::getLocalDayOrdinal(currentTimestamp);
  const uint32_t savedDay =
      TimeUtils::getLocalDayOrdinal(APP_STATE.lastKnownValidTimestamp);

  return currentDay != 0 && currentDay == savedDay;
}

bool prerequisitesStillAllowAttempt() {
  if (!SETTINGS.autoSyncDay) {
    return false;
  }

  if (alreadySynchronizedForCurrentDay()) {
    LOG_DBG("TIME", "Auto Sync Day skipped: current day is already trusted");
    return false;
  }

  if (!WIFI_STORE.loadFromFile() || !WIFI_STORE.hasCredentials()) {
    LOG_DBG("TIME", "Auto Sync Day skipped: no saved Wi-Fi network");
    return false;
  }

  return true;
}

void beginScan() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false);
  WiFi.scanDelete();

  const int16_t startResult = WiFi.scanNetworks(true);
  if (startResult == WIFI_SCAN_FAILED) {
    LOG_DBG("TIME", "Auto Sync Day: could not start Wi-Fi scan");
    finishAttempt();
    return;
  }

  candidate = Candidate{};
  deadlineMs = millis() + SCAN_TIMEOUT_MS;
  state = State::SCANNING;
  LOG_DBG("TIME", "Auto Sync Day: background Wi-Fi scan started");
}

void beginConnection() {
  const char* password =
      candidate.credential.password.empty()
          ? nullptr
          : candidate.credential.password.c_str();

  if (candidate.hasBssid) {
    WiFi.begin(candidate.credential.ssid.c_str(), password,
               candidate.channel, candidate.bssid);
  } else if (password != nullptr) {
    WiFi.begin(candidate.credential.ssid.c_str(), password);
  } else {
    WiFi.begin(candidate.credential.ssid.c_str());
  }

  deadlineMs = millis() + CONNECT_TIMEOUT_MS;
  state = State::CONNECTING;
  LOG_DBG("TIME", "Auto Sync Day: connecting to saved network");
}

void beginNtp() {
  TimeUtils::startNtpSync();
  deadlineMs = millis() + NTP_TIMEOUT_MS;
  state = State::NTP;
  LOG_DBG("TIME", "Auto Sync Day: background NTP started");
}

}  // namespace

void SilentTimeSync::schedule(const bool allowWifiAttempt) {
  state = State::IDLE;
  candidate = Candidate{};
  deadlineMs = 0;

  if (!allowWifiAttempt || !SETTINGS.autoSyncDay) {
    return;
  }

  // No SD/Wi-Fi work here. setup() can finish and render Home/Reader first.
  deadlineMs = millis() + POST_BOOT_DELAY_MS;
  state = State::WAITING;
  LOG_DBG("TIME", "Auto Sync Day scheduled after boot");
}

bool SilentTimeSync::tick() {
  if (state == State::IDLE) {
    return false;
  }

  const uint32_t now = millis();

  switch (state) {
    case State::WAITING:
      if (!deadlineReached(now, deadlineMs)) {
        return false;
      }

      TimeUtils::configureTimezone();

      if (!prerequisitesStillAllowAttempt()) {
        state = State::IDLE;
        return false;
      }

      // Never steal Wi-Fi from a foreground activity.
      if (WiFi.getMode() != WIFI_MODE_NULL) {
        deadlineMs = now + USER_ACTIVITY_RETRY_DELAY_MS;
        return false;
      }

      beginScan();
      return false;

    case State::SCANNING: {
      const int16_t result = WiFi.scanComplete();

      if (result == WIFI_SCAN_FAILED) {
        LOG_DBG("TIME", "Auto Sync Day: Wi-Fi scan failed");
        finishAttempt();
        return false;
      }

      if (result >= 0) {
        if (!selectCandidate(candidate)) {
          LOG_DBG("TIME", "Auto Sync Day: no saved network in range");
          finishAttempt();
          return false;
        }

        beginConnection();
        return false;
      }

      if (deadlineReached(now, deadlineMs)) {
        LOG_DBG("TIME", "Auto Sync Day: Wi-Fi scan timed out");
        finishAttempt();
      }
      return false;
    }

    case State::CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        beginNtp();
        return false;
      }

      if (deadlineReached(now, deadlineMs)) {
        LOG_DBG("TIME", "Auto Sync Day: Wi-Fi connection timed out");
        finishAttempt();
      }
      return false;

    case State::NTP:
      if (TimeUtils::pollNtpSync()) {
        const uint32_t syncedTimestamp =
            TimeUtils::getCurrentValidTimestamp();

        if (TimeUtils::isClockValid(syncedTimestamp)) {
          APP_STATE.registerValidTimeSync(syncedTimestamp);
          APP_STATE.saveToFile();
          WIFI_STORE.setLastConnectedSsid(candidate.credential.ssid);
          LOG_DBG("TIME", "Auto Sync Day: background synchronization complete");
          finishAttempt();
          return true;
        }

        LOG_DBG("TIME", "Auto Sync Day: NTP completed without a valid clock");
        finishAttempt();
        return false;
      }

      if (deadlineReached(now, deadlineMs)) {
        LOG_DBG("TIME", "Auto Sync Day: NTP timed out");
        finishAttempt();
      }
      return false;

    case State::IDLE:
    default:
      return false;
  }
}

void SilentTimeSync::notifyUserActivity() {
  if (state == State::IDLE) {
    return;
  }

  if (state == State::WAITING) {
    deadlineMs = millis() + USER_ACTIVITY_RETRY_DELAY_MS;
    return;
  }

  LOG_DBG("TIME", "Auto Sync Day: foreground activity detected, postponing");
  postponeAfterUserActivity();
}

bool SilentTimeSync::isPendingOrRunning() {
  return state != State::IDLE;
}
