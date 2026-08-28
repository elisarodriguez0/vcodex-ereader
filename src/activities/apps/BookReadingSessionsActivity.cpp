#include "BookReadingSessionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>

#include "AppMetricCard.h"
#include "ReadingDayDetailActivity.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HeaderDateUtils.h"
#include "util/ReadingStatsAnalytics.h"

namespace {

constexpr int CARD_HEIGHT = 78;
constexpr int CARD_GAP = 8;

const ReadingBookStats* findBook(const std::string& bookPath) {
  for (const auto& book : READING_STATS.getBooks()) {
    if (book.path == bookPath) {
      return &book;
    }

    for (const auto& knownPath : book.knownPaths) {
      if (knownPath == bookPath) {
        return &book;
      }
    }
  }

  return nullptr;
}

bool sessionBelongsToBook(const ReadingSessionLogEntry& session, const ReadingBookStats& book) {
  if (!book.bookId.empty() && !session.bookId.empty() && session.bookId == book.bookId) {
    return true;
  }

  if (!session.path.empty()) {
    if (session.path == book.path) {
      return true;
    }

    for (const auto& knownPath : book.knownPaths) {
      if (session.path == knownPath) {
        return true;
      }
    }
  }

  return false;
}

uint32_t countSessionsForDay(const ReadingBookStats& book, const uint32_t dayOrdinal) {
  uint32_t count = 0;

  for (const auto& session : READING_STATS.getSessionLog()) {
    if (session.dayOrdinal == dayOrdinal && sessionBelongsToBook(session, book)) {
      ++count;
    }
  }

  return count;
}

std::string getBookTitle(const ReadingBookStats& book) {
  return book.title.empty() ? book.path : book.title;
}

std::string buildSessionLabel(const uint32_t sessions) {
  return std::to_string(sessions) + " " +
         (sessions == 1 ? std::string(tr(STR_SESSION)) : std::string(tr(STR_SESSIONS)));
}

void drawDayCard(GfxRenderer& renderer, const Rect& rect, const BookReadingSessionsActivity::DayEntry& entry,
                 const bool selected) {
  AppMetricCard::Options options;
  options.valueLargeY = 14;
  options.labelY = 50;
  options.shrinkValue = true;
  options.labelMode = AppMetricCard::LabelMode::Simple;

  const std::string date = ReadingStatsAnalytics::formatDayOrdinalLabel(entry.dayOrdinal);
  const std::string value = date + " · " + ReadingStatsAnalytics::formatDurationHm(entry.readingMs);
  const std::string label = buildSessionLabel(entry.sessions);

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::MediumGray);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);

    const int textWidth = rect.width - options.contentInset;
    const int valueFontId =
        options.shrinkValue &&
                renderer.getTextWidth(UI_12_FONT_ID, value.c_str(), EpdFontFamily::BOLD) > textWidth
            ? UI_10_FONT_ID
            : UI_12_FONT_ID;
    const std::string truncatedValue =
        renderer.truncatedText(valueFontId, value.c_str(), textWidth, EpdFontFamily::BOLD);
    renderer.drawText(valueFontId, rect.x + options.paddingX,
                      rect.y + (valueFontId == UI_12_FONT_ID ? options.valueLargeY : options.valueSmallY),
                      truncatedValue.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, rect.x + options.paddingX, rect.y + options.labelY, label.c_str());
    return;
  }

  AppMetricCard::draw(renderer, rect, label.c_str(), value, options);
}

}  // namespace

void BookReadingSessionsActivity::refreshEntries() {
  entries.clear();

  const auto* book = findBook(bookPath);
  if (!book) {
    selectedIndex = 0;
    return;
  }

  entries.reserve(book->readingDays.size());

  // Newest day first.
  for (auto it = book->readingDays.rbegin(); it != book->readingDays.rend(); ++it) {
    if (it->dayOrdinal == 0 || it->readingMs == 0) {
      continue;
    }

    entries.push_back(DayEntry{
        it->dayOrdinal,
        it->readingMs,
        countSessionsForDay(*book, it->dayOrdinal),
    });
  }

  if (entries.empty()) {
    selectedIndex = 0;
  } else {
    selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(entries.size()) - 1);
  }
}

void BookReadingSessionsActivity::openSelectedDay() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(entries.size())) {
    return;
  }

  startActivityForResult(
      std::make_unique<ReadingDayDetailActivity>(renderer, mappedInput, entries[selectedIndex].dayOrdinal),
      [this](const ActivityResult&) {
        refreshEntries();
        requestUpdate();
      });
}

void BookReadingSessionsActivity::onEnter() {
  Activity::onEnter();
  refreshEntries();
  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void BookReadingSessionsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (waitForConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      waitForConfirmRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedDay();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    if (entries.empty()) {
      return;
    }

    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(entries.size()));
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    if (entries.empty()) {
      return;
    }

    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(entries.size()));
    requestUpdate();
  });
}

void BookReadingSessionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = metrics.contentSidePadding;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int availableHeight = std::max(1, contentBottom - contentTop);
  const int cardStride = CARD_HEIGHT + CARD_GAP;
  const int visibleCount = std::max(1, (availableHeight + CARD_GAP) / cardStride);

  const auto* book = findBook(bookPath);
  const std::string subtitle = book ? getBookTitle(*book) : std::string();

  HeaderDateUtils::drawHeaderWithDate(renderer, tr(STR_SESSIONS), subtitle.c_str());

  if (entries.empty()) {
    renderer.drawText(UI_10_FONT_ID, sidePadding, contentTop + 20, tr(STR_NO_READING_STATS));
  } else {
    int firstVisible = selectedIndex - visibleCount + 1;
    firstVisible = std::max(0, firstVisible);
    firstVisible = std::min(firstVisible, std::max(0, static_cast<int>(entries.size()) - visibleCount));

    int y = contentTop;
    for (int i = firstVisible; i < static_cast<int>(entries.size()) && i < firstVisible + visibleCount; ++i) {
      drawDayCard(renderer, Rect{sidePadding, y, pageWidth - sidePadding * 2, CARD_HEIGHT}, entries[i],
                  i == selectedIndex);
      y += cardStride;
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), entries.empty() ? "" : tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
