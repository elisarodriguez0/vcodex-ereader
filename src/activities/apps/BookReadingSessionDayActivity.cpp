#include "BookReadingSessionDayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "AppMetricCard.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HeaderDateUtils.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"

namespace {
constexpr int CARD_HEIGHT = 78;
constexpr int CARD_GAP = 8;

const ReadingBookStats* findBook(const std::string& bookPath) {
  for (const auto& book : READING_STATS.getBooks()) {
    if (book.path == bookPath) return &book;
    for (const auto& knownPath : book.knownPaths) {
      if (knownPath == bookPath) return &book;
    }
  }
  return nullptr;
}

bool sessionBelongsToBook(const ReadingSessionLogEntry& session, const ReadingBookStats& book) {
  if (!book.bookId.empty() && !session.bookId.empty() && session.bookId == book.bookId) return true;
  if (session.path == book.path) return true;
  return std::find(book.knownPaths.begin(), book.knownPaths.end(), session.path) != book.knownPaths.end();
}

std::string formatClock(const uint32_t timestamp) {
  if (!TimeUtils::isClockValid(timestamp)) return "";
  const time_t value = static_cast<time_t>(timestamp);
  struct tm local{};
  localtime_r(&value, &local);
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d", local.tm_hour, local.tm_min);
  return std::string(buffer);
}

std::string formatSessionValue(const uint32_t startAt, const uint32_t endAt, const uint32_t sessionMs) {
  return formatClock(startAt) + "–" + formatClock(endAt) + " · " + ReadingStatsAnalytics::formatDurationHm(sessionMs);
}

std::string periodLabel(const uint32_t morningMs, const uint32_t afternoonMs, const uint32_t nightMs) {
  std::string result;
  auto append = [&](const char* value) {
    if (!result.empty()) result += " + ";
    result += value;
  };
  if (morningMs > 0) append("Mañana");
  if (afternoonMs > 0) append("Tarde");
  if (nightMs > 0) append("Noche");
  return result.empty() ? std::string("Hora registrada") : result;
}

void drawSessionCard(GfxRenderer& renderer, const Rect& rect, const std::string& value, const std::string& label) {
  AppMetricCard::Options options;
  options.valueLargeY = 14;
  options.labelY = 50;
  options.shrinkValue = true;
  options.labelMode = AppMetricCard::LabelMode::Simple;
  AppMetricCard::draw(renderer, rect, label.c_str(), value, options);
}
}  // namespace

void BookReadingSessionDayActivity::refreshEntries() {
  timedSessions.clear();
  untimedSessionCount = 0;

  const auto* book = findBook(bookPath);
  if (!book) return;

  for (const auto& session : READING_STATS.getSessionLog()) {
    if (session.dayOrdinal != dayOrdinal || !sessionBelongsToBook(session, *book)) continue;

    if (TimeUtils::isClockValid(session.startAt) && TimeUtils::isClockValid(session.endAt) &&
        session.endAt >= session.startAt) {
      timedSessions.push_back(TimedSessionEntry{session.startAt, session.endAt, session.sessionMs, session.morningMs,
                                                session.afternoonMs, session.nightMs});
    } else {
      ++untimedSessionCount;
    }
  }

  std::sort(timedSessions.begin(), timedSessions.end(), [](const TimedSessionEntry& left, const TimedSessionEntry& right) {
    return left.startAt < right.startAt;
  });
  firstVisible = 0;
}

void BookReadingSessionDayActivity::onEnter() {
  Activity::onEnter();
  refreshEntries();
  requestUpdate();
}

void BookReadingSessionDayActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int visibleCount = std::max(1, (contentBottom - contentTop + CARD_GAP) / (CARD_HEIGHT + CARD_GAP));
  const int totalEntries = static_cast<int>(timedSessions.size()) + (untimedSessionCount > 0 ? 1 : 0);
  const int maxFirst = std::max(0, totalEntries - visibleCount);

  buttonNavigator.onPreviousRelease([&]() {
    if (firstVisible > 0) {
      --firstVisible;
      requestUpdate();
    }
  });
  buttonNavigator.onNextRelease([&]() {
    if (firstVisible < maxFirst) {
      ++firstVisible;
      requestUpdate();
    }
  });
}

void BookReadingSessionDayActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int sidePadding = metrics.contentSidePadding;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int visibleCount = std::max(1, (contentBottom - contentTop + CARD_GAP) / (CARD_HEIGHT + CARD_GAP));
  const int totalEntries = static_cast<int>(timedSessions.size()) + (untimedSessionCount > 0 ? 1 : 0);
  const int maxFirst = std::max(0, totalEntries - visibleCount);
  firstVisible = std::clamp(firstVisible, 0, maxFirst);

  HeaderDateUtils::drawHeaderWithDate(renderer, tr(STR_SESSIONS), ReadingStatsAnalytics::formatDayOrdinalLabel(dayOrdinal).c_str());

  if (totalEntries == 0) {
    renderer.drawText(UI_10_FONT_ID, sidePadding, contentTop + 20, "No hay horas de sesión registradas");
  } else {
    int y = contentTop;
    for (int index = firstVisible; index < totalEntries && index < firstVisible + visibleCount; ++index) {
      if (index < static_cast<int>(timedSessions.size())) {
        const auto& session = timedSessions[index];
        drawSessionCard(renderer, Rect{sidePadding, y, pageWidth - sidePadding * 2, CARD_HEIGHT},
                        formatSessionValue(session.startAt, session.endAt, session.sessionMs),
                        periodLabel(session.morningMs, session.afternoonMs, session.nightMs));
      } else {
        const std::string value = std::to_string(untimedSessionCount) +
                                  (untimedSessionCount == 1 ? " sesión" : " sesiones");
        drawSessionCard(renderer, Rect{sidePadding, y, pageWidth - sidePadding * 2, CARD_HEIGHT}, value,
                        "Histórico sin hora");
      }
      y += CARD_HEIGHT + CARD_GAP;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", firstVisible > 0 ? tr(STR_DIR_UP) : "",
                                            firstVisible < maxFirst ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
