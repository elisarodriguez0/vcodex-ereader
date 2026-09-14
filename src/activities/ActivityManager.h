#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

/**
 * Activity lifecycle management system (Android-inspired).
 * 
 * An activity represents a single screen of the UI (e.g., Reader, Settings, File Browser).
 * The ActivityManager coordinates:
 * - Activity lifecycle (begin, loop, exit)
 * - Activity stack navigation (push, pop, replace)
 * - Rendering synchronization between main task and render task
 * - Input event routing to the current activity
 * - Memory and resource cleanup when activities transition
 * 
 * Key Differences from Android:
 * - No onPause/onResume (no background activities; one screen at a time)
 * - No intent system (data passed directly to activities)
 * - Activity results returned via callbacks instead of separate methods
 * - Dedicated render task handles all screen updates (separate from main event loop)
 * 
 * Threading Model:
 * - Main loop: Handles input, activity logic, and render requests
 * - Render task: Continuous rendering of current activity
 * - Synchronization via renderingMutex (protect via RenderLock)
 * 
 * Activity Stack:
 * - currentActivity: The displayed screen
 * - stackActivities: History for back navigation (goTo... functions clear stack)
 * - Launching activity: Main task queues pending activity + action
 * - Render task processes pending: Calls onExit on old, onCreate on new
 * 
 * Memory Constraints:
 * Devices typically allocate 50-100 KB per activity. Stack size is limited to ~10.
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  /* Exit current activity and process pending activity transition */
  void exitActivity(const RenderLock& lock);

  /* Activity queued for launch on next render loop iteration */
  std::unique_ptr<Activity> pendingActivity;

  /**
   * Pending activity operation enumeration.
   */
  enum class PendingAction {
    None,     /**< No pending activity change */
    Push,     /**< Push pending activity onto stack (back button available) */
    Pop,      /**< Pop current activity, return to previous */
    Replace   /**< Replace current activity (clear stack) */
  };
  PendingAction pendingAction = PendingAction::None;

  /* Task handle for dedicated render loop */
  TaskHandle_t renderTaskHandle = nullptr;

  /* Trampoline function for render task */
  static void renderTaskTrampoline(void* param);

  /**
   * Dedicated render task loop (runs on separate CPU core).
   * Continuously calls currentActivity->render() and syncs display.
   * This is marked [[noreturn]] as render task runs indefinitely.
   */
  [[noreturn]] virtual void renderTaskLoop();

  /**
   * Main task waiting for render to complete.
   * Set by requestUpdateAndWait(), cleared after render.
   * Note: Only one task can wait at a time.
   */
  TaskHandle_t waitingTaskHandle = nullptr;

  /**
   * Mutex protecting rendering operations.
   * Must only be accessed via RenderLock (RAII pattern).
   * Prevents race conditions between main and render tasks.
   */
  SemaphoreHandle_t renderingMutex = nullptr;

  /**
   * Flag: Render requested after current loop iteration.
   * Set by requestUpdate(). Only the main loop modifies this.
   */
  bool requestedUpdate = false;

  /**
   * Accumulated screen refresh "debt" from auto-refresh frequency.
   * Used to throttle render requests without dropping frame changes.
   */
  uint8_t autoUiRefreshDebt = 0;

  /**
   * Weight of previous UI refresh (for smooth activity transitions).
   */
  uint8_t deferredPreviousUiRefreshWeight = 0;

  /**
   * Schedule a render with transition weight.
   * Ensures smooth visual transition between activities.
   */
  void requestUiTransitionRefresh(uint8_t previousWeight, uint8_t nextWeight);

 public:
  /**
   * Construct ActivityManager with renderer and input manager.
   * Initializes renderingMutex and reserves space for activity stack.
   * 
   * @param renderer GfxRenderer instance for screen updates
   * @param mappedInput MappedInputManager for button input routing
   */
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }

  /**
   * Destructor (never called on embedded device).
   */
  ~ActivityManager() { assert(false); /* should never be called */ };

  /**
   * Initialize ActivityManager at startup.
   * Launches the initial activity (typically HomeActivity).
   * Creates the dedicated render task.
   */
  void begin();

  /**
   * Main event loop iteration.
   * - Routes input to current activity
   * - Processes activity transitions (push, pop, replace)
   * - Requests render updates
   * - Manages auto-sleep timeout
   * Call this repeatedly in main FreeRTOS task.
   */
  void loop();

  /**
   * Replace current activity with a new one, clearing the entire stack.
   * 
   * Used for major navigations (e.g., Settings → Home) where back button
   * should not return to the previous screen.
   * 
   * @param newActivity Activity to launch (replaces current)
   */
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  /* Convenience functions for common activity transitions */

  /**
   * Navigate to file transfer activity.
   * Replaces current activity.
   */
  void goToFileTransfer();

  /**
   * Navigate to settings activity.
   * Replaces current activity.
   */
  void goToSettings();

  /**
   * Navigate to apps launcher activity.
   * Replaces current activity.
   */
  void goToApps();

  /**
   * Navigate to file browser activity.
   * 
   * @param path Initial directory to browse (empty = default library path)
   */
  void goToFileBrowser(std::string path = {});

  /**
   * Navigate to recent books list activity.
   * Replaces current activity.
   */
  void goToRecentBooks();

  /**
   * Navigate to web browser activity.
   * Replaces current activity.
   */
  void goToBrowser();

  /**
   * Navigate to e-book reader activity.
   * 
   * @param path Path to EPUB/TXT file on SD card
   */
  void goToReader(std::string path);

  /**
   * Navigate to KOReader sync activity (calibre integration).
   * Replaces current activity.
   */
  void goToKOReaderSync();

  /**
   * Navigate to reader with specific bookmark/location.
   * Used for jumping to a specific chapter/page.
   * 
   * @param path Path to EPUB file
   * @param spineIndex Spine index within EPUB (chapter)
   * @param page Page number within spine
   * @param hasVisibleTextOffset Whether page has text offset
   * @param visibleTextOffset Text offset on page
   */
  void goToEpubBookmark(std::string path, int spineIndex, uint32_t page, bool hasVisibleTextOffset = false,
                        uint32_t visibleTextOffset = 0);

  /**
   * Navigate to sleep/screensaver activity.
   * Replaces current activity. Device display goes to sleep mode.
   */
  void goToSleep();

  /**
   * Navigate to boot activity (splash screen).
   * Used during device startup.
   */
  void goToBoot();

  /**
   * Display a full-screen message activity.
   * Replaces current activity. Used for alerts, notifications.
   * 
   * @param message Text to display
   * @param style Font style for display
   */
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);

  /**
   * Navigate to crash report activity.
   * Shows diagnostic information when an error occurs.
   */
  void goToCrashReport();

  /**
   * Navigate to home activity.
   * Replaces current activity. Clears activity stack.
   */
  void goHome();

  /**
   * Push activity onto the stack (preserves current for back navigation).
   * 
   * Use for sub-activities that return to the caller:
   * - WiFi network selection from file transfer screen
   * - Settings dialogs
   * 
   * User pressing back button calls popActivity().
   * 
   * @param activity Activity to push onto stack
   */
  void pushActivity(std::unique_ptr<Activity>&& activity);

  /**
   * Pop current activity from stack, returning to previous.
   * 
   * If stack is empty, navigates to home activity.
   * Called when user presses back button or activity completes.
   */
  void popActivity();

  /**
   * Check if current activity prevents auto-sleep.
   * Some activities (reader, web server) disable sleep mode.
   * 
   * @return true if auto-sleep is disabled, false if normal timeout applies
   */
  bool preventAutoSleep() const;

  /**
   * Check if current activity is the reader activity.
   * Used to suppress certain UI elements or features outside of reading.
   * 
   * @return true if reader is active, false otherwise
   */
  bool isReaderActivity() const;

  /**
   * Process forced refresh requests (e.g., from user button combination).
   * 
   * @return true if refresh was handled, false otherwise
   */
  bool handleForcedRefresh();

  /**
   * Check if render loop should skip normal delay.
   * Some activities may request high-frequency rendering.
   * 
   * @return true if delay should be skipped, false for normal throttling
   */
  bool skipLoopDelay() const;

  /**
   * Get screenshot information for current activity.
   * Used by screenshot capture feature.
   */
  ScreenshotInfo getScreenshotInfo() const;

  /**
   * Request a screen render update.
   * 
   * Queues an update to be performed asynchronously. If immediate=true,
   * the update is triggered right away (blocking).
   * 
   * @param immediate If true, perform render immediately; if false, defer to end of loop
   */
  void requestUpdate(bool immediate = false);

  /**
   * Request render update and block until it completes.
   * 
   * Synchronously waits for the render task to complete one full render cycle.
   * MUST NOT be called from the render task or while holding RenderLock.
   * Useful for ensuring display is updated before continuing critical operations.
   */
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
