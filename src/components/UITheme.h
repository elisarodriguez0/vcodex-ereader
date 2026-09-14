#pragma once

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

/**
 * Singleton manager for UI theme and rendering metrics.
 * 
 * Provides a facade to the active theme implementation and layout metrics.
 * Themes are selectable from CrossPointSettings (LYRA, LYRA_CUSTOM, LYRA_CAROUSEL).
 * 
 * Responsibilities:
 * - Load and switch themes based on user settings
 * - Provide access to theme rendering methods
 * - Calculate layout dimensions (items per page, cover thumbnail sizes)
 * - Map filenames to appropriate UI icons
 * - Provide status bar and progress bar dimensions
 */
class UITheme {
  /* Static singleton instance */
  static UITheme instance;

 public:
  /**
   * Initialize the UITheme singleton with the default theme.
   */
  UITheme();

  /**
   * Get the singleton instance of UITheme.
   */
  static UITheme& getInstance() { return instance; }

  /**
   * Get the current theme metrics (layout dimensions, spacing, colors).
   */
  const ThemeMetrics& getMetrics() const { return *currentMetrics; }

  /**
   * Get the active theme implementation for rendering.
   */
  const BaseTheme& getTheme() const { return *currentTheme; }

  /**
   * Reload the current theme from settings.
   * Called when user changes theme preference in settings.
   */
  void reload();

  /**
   * Switch to a specific theme by type.
   * 
   * @param type Theme type from CrossPointSettings (LYRA, LYRA_CUSTOM, LYRA_CAROUSEL)
   */
  void setTheme(CrossPointSettings::UI_THEME type);

  /**
   * Calculate how many list items fit on a page.
   * 
   * Accounts for header, tab bar, button hints, and subtitle.
   * Used for pagination in library and recent books screens.
   * 
   * @param renderer Active renderer for device capabilities
   * @param hasHeader Whether page has a header section
   * @param hasTabBar Whether page has tab navigation
   * @param hasButtonHints Whether page has button action hints
   * @param hasSubtitle Whether items have subtitle text
   * @param extraReservedHeight Additional vertical space reserved for custom content
   * @return Number of items that fit on one page
   */
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);

  /**
   * Get the path to a cover thumbnail (square crop).
   * Used for list item thumbnails in library screens.
   * 
   * @param coverBmpPath Original cover image path on SD card
   * @param coverHeight Desired thumbnail size (width = height for square)
   * @return Path to cached thumbnail file
   */
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);

  /**
   * Get the path to a cover thumbnail (rectangular crop).
   * Allows different width and height for book cover aspect ratios.
   * 
   * @param coverBmpPath Original cover image path on SD card
   * @param coverWidth Desired thumbnail width
   * @param coverHeight Desired thumbnail height
   * @return Path to cached thumbnail file
   */
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverWidth, int coverHeight);

  /**
   * Get the icon to display for a file.
   * 
   * Maps file extensions to appropriate UI icons (book, folder, image, document, etc.).
   * Used in file browser and library screens.
   * 
   * @param filename File name or path
   * @return Icon identifier for rendering
   */
  static UIIcon getFileIcon(const std::string& filename);

  /**
   * Get the height (in pixels) of the status bar.
   * Status bar includes battery, time, connection, etc.
   */
  static int getStatusBarHeight();

  /**
   * Get the height (in pixels) of the reading progress bar.
   * Displayed at bottom of reading screen.
   */
  static int getProgressBarHeight();

 private:
  const ThemeMetrics* currentMetrics;       /**< Layout dimensions and spacing metrics */
  std::unique_ptr<BaseTheme> currentTheme;  /**< Active theme renderer implementation */
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
