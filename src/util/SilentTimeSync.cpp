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

constexpr uint32_t SCAN_TIMEOUT_MS = 6000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
constexpr uint32_t NTP_TIMEOUT_MS = 5000;

struct Candidate {
  WifiCredential credential;
  int32_t rssi = INT32_MIN;
  int32_t channel = 0;
  uint8_t bssid[6] = {};
  bool valid = false;
  bool hasBssid = false;
  bool preferred = false;
};

void shutDownWifi() {
  TimeUtils::stopNtp();
  WiFi.scanDelete();
  WiFi.disconnect(false);
  delay(50);
  WiFi.mode(WIFI_OFF);
  delay(50);
}

bool waitForScan() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true);

  const uint32_t started = millis();
  while (millis() - started < SCAN_TIMEOUT_MS) {
    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_FAILED) {
      return false;
    }
    if (result >= 0) {
      return true;
    }
    delay(50);
  }

  return false;
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

}  // namespace

bool SilentTimeSync::run(const bool allowWifiAttempt) {
  TimeUtils::configureTimezone();

  if (alreadySynchronizedForCurrentDay()) {
    LOG_DBG("TIME", "Auto Sync Day skipped: current day is already trusted");
    return false;
  }

  if (!allowWifiAttempt || !SETTINGS.autoSyncDay) {
    return false;
  }

  if (!WIFI_STORE.loadFromFile() || !WIFI_STORE.hasCredentials()) {
    LOG_DBG("TIME", "Auto Sync Day skipped: no saved Wi-Fi network");
    return false;
  }

  LOG_DBG("TIME", "Auto Sync Day: starting silent saved-network sync");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false);
  delay(50);

  Candidate candidate;
  if (!waitForScan() || !selectCandidate(candidate)) {
    LOG_DBG("TIME", "Auto Sync Day: no saved network in range");
    shutDownWifi();
    return false;
  }

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

  const uint32_t connectStarted = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - connectStarted < CONNECT_TIMEOUT_MS) {
    delay(50);
  }

  if (WiFi.status() != WL_CONNECTED) {
    LOG_DBG("TIME", "Auto Sync Day: Wi-Fi connection timed out");
    shutDownWifi();
    return false;
  }

  const bool synchronized = TimeUtils::syncTimeWithNtp(NTP_TIMEOUT_MS);
  const uint32_t syncedTimestamp = TimeUtils::getCurrentValidTimestamp();

  if (synchronized && TimeUtils::isClockValid(syncedTimestamp)) {
    APP_STATE.registerValidTimeSync(syncedTimestamp);
    APP_STATE.saveToFile();
    WIFI_STORE.setLastConnectedSsid(candidate.credential.ssid);
    LOG_DBG("TIME", "Auto Sync Day: synchronization complete");
  } else {
    LOG_DBG("TIME", "Auto Sync Day: NTP timed out");
  }

  shutDownWifi();
  return synchronized;
}
