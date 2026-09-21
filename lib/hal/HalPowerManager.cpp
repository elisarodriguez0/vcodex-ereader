#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {

// X4 has no fuel gauge. These are the current FreeInk 1S Li-ion rest-voltage
// anchors, deliberately exposed as 10% notches instead of fake 1% precision.
// The X3 path below still uses its hardware BQ27220 SoC reading.
constexpr uint16_t X4_LIION_NOTCH_MV[11] = {
    3450,  //   0%
    3680,  //  10%
    3740,  //  20%
    3770,  //  30%
    3790,  //  40%
    3820,  //  50%
    3870,  //  60%
    3920,  //  70%
    3980,  //  80%
    4060,  //  90%
    4200,  // 100%
};

constexpr uint16_t X4_NOTCH_HYSTERESIS_MV = 8;
constexpr uint16_t X4_MIN_VALID_MV = 2500;
constexpr uint16_t X4_MAX_VALID_MV = 4500;
constexpr size_t X4_ADC_SAMPLE_COUNT = 5;

uint16_t x4PercentFromMillivolts(uint16_t millivolts) {
  if (millivolts >= X4_LIION_NOTCH_MV[10]) return 100;

  for (uint8_t i = 10; i > 0; --i) {
    const uint16_t boundary =
        static_cast<uint16_t>((X4_LIION_NOTCH_MV[i - 1] + X4_LIION_NOTCH_MV[i]) / 2);
    if (millivolts >= boundary) return static_cast<uint16_t>(i * 10);
  }
  return 0;
}

uint16_t x4PercentFromMillivolts(uint16_t millivolts, uint16_t previousPercent) {
  const uint16_t notch = x4PercentFromMillivolts(millivolts);
  if (previousPercent > 100) return notch;

  const uint16_t previousNotch = static_cast<uint16_t>((previousPercent / 10) * 10);
  if (notch == previousNotch) return notch;

  const int32_t bias = notch > previousNotch ? -X4_NOTCH_HYSTERESIS_MV : X4_NOTCH_HYSTERESIS_MV;
  const int32_t biased =
      std::clamp<int32_t>(static_cast<int32_t>(millivolts) + bias, 0, UINT16_MAX);

  return x4PercentFromMillivolts(static_cast<uint16_t>(biased)) == notch ? notch : previousNotch;
}

bool readStableX4Millivolts(const BatteryMonitor& battery, uint16_t& out) {
  uint16_t samples[X4_ADC_SAMPLE_COUNT] = {};
  size_t valid = 0;

  for (size_t i = 0; i < X4_ADC_SAMPLE_COUNT; ++i) {
    const uint16_t mv = battery.readMillivolts();
    if (mv >= X4_MIN_VALID_MV && mv <= X4_MAX_VALID_MV) {
      samples[valid++] = mv;
    }
    if (i + 1 < X4_ADC_SAMPLE_COUNT) delay(2);
  }

  if (valid < 3) return false;

  std::sort(samples, samples + valid);
  out = samples[valid / 2];
  return true;
}

}  // namespace

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    enabled = false;
  }

  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#ifdef ENABLE_SERIAL_LOG
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (!gpio.deviceIsX3()) {
    constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
    gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SPIWP, 0);
    gpio_hold_en(GPIO_SPIWP);
  }
#endif

  freeink::PowerManager::powerDownRailsForSleep();
  freeink::PowerManager::deepSleepUntilPowerButton();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;

  const unsigned long now = millis();
  if (_batteryHasSample && _batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
    return static_cast<uint16_t>(_batteryCachedPercent);
  }
  _batteryLastPollMs = now;

  // X3: real BQ27220 fuel gauge -> use hardware SoC directly.
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryHasSample ? static_cast<uint16_t>(_batteryCachedPercent) : 0;
    }
    _batteryCachedPercent = std::min<uint16_t>(percent, 100);
    _batteryHasSample = true;
    return static_cast<uint16_t>(_batteryCachedPercent);
  }

  // X4: no fuel gauge. Reject impossible ADC samples, median-filter the rest,
  // then use the current FreeInk Li-ion notches + hysteresis.
  uint16_t millivolts = 0;
  if (!readStableX4Millivolts(battery, millivolts)) {
    return _batteryHasSample ? static_cast<uint16_t>(_batteryCachedPercent) : 0;
  }

  const uint16_t previous =
      _batteryHasSample ? static_cast<uint16_t>(_batteryCachedPercent) : UINT16_MAX;
  _batteryCachedPercent = x4PercentFromMillivolts(millivolts, previous);
  _batteryHasSample = true;
  return static_cast<uint16_t>(_batteryCachedPercent);
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
