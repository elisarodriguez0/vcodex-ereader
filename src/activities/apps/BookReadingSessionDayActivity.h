#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class BookReadingSessionDayActivity final : public Activity {
  struct TimedSessionEntry {
    uint32_t startAt = 0;
    uint32_t endAt = 0;
    uint32_t sessionMs = 0;
    uint32_t morningMs = 0;
    uint32_t afternoonMs = 0;
    uint32_t nightMs = 0;
  };

  ButtonNavigator buttonNavigator;
  std::string bookPath;
  uint32_t dayOrdinal = 0;
  std::vector<TimedSessionEntry> timedSessions;
  uint32_t untimedSessionCount = 0;
  int firstVisible = 0;

  void refreshEntries();

 public:
  BookReadingSessionDayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                                const uint32_t dayOrdinal)
      : Activity("BookReadingSessionDay", renderer, mappedInput),
        bookPath(std::move(bookPath)),
        dayOrdinal(dayOrdinal) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
