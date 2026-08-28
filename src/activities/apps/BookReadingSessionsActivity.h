#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class BookReadingSessionsActivity final : public Activity {
 public:
  struct DayEntry {
    uint32_t dayOrdinal = 0;
    uint64_t readingMs = 0;
    uint32_t sessions = 0;
  };

 private:
  ButtonNavigator buttonNavigator;
  std::string bookPath;
  std::vector<DayEntry> entries;
  int selectedIndex = 0;
  bool waitForConfirmRelease = false;

  void refreshEntries();
  void openSelectedDay();

 public:
  explicit BookReadingSessionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath)
      : Activity("BookReadingSessions", renderer, mappedInput), bookPath(std::move(bookPath)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
