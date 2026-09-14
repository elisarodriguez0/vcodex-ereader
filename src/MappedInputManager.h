#pragma once

#include <HalGPIO.h>

/**
 * Abstraction layer for hardware button input with button remapping support.
 * 
 * Provides a logical button interface independent of physical GPIO pin assignments.
 * Supports:
 * - Front button layout remapping (back, confirm, left, right)
 * - Side button layout remapping (page back/forward)
 * - Button state queries (pressed, released, held duration)
 * - Confirm button release guard (prevents accidental double-triggering)
 * 
 * The X4 device allows users to remap front buttons via CrossPointSettings.
 * This manager translates logical button enums to physical GPIO pins at runtime.
 */
class MappedInputManager {
 public:
  /**
   * Logical button identifiers.
   * 
   * BACK, CONFIRM, LEFT, RIGHT: Front buttons (subject to remapping)
   * UP, DOWN: Alternative navigation buttons
   * POWER: Power button
   * PAGEBACK, PAGEFORWARD: Side buttons (subject to remapping)
   */
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  /**
   * Button label mapping for UI display.
   * Used to show correct labels in settings/help after remapping.
   */
  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  /**
   * Update internal button state from GPIO hardware.
   * Must be called every frame before querying button state.
   */
  void update() const { gpio.update(); }

  /**
   * Arm the confirm release guard to suppress false triggers.
   * Prevents accidental confirmations from debounce noise.
   */
  void armConfirmReleaseGuard() const;

  /**
   * Query whether a button transitioned from not-pressed to pressed this frame.
   */
  bool wasPressed(Button button) const;

  /**
   * Query whether a button transitioned from pressed to not-pressed this frame.
   */
  bool wasReleased(Button button) const;

  /**
   * Query the current button state (pressed or not).
   */
  bool isPressed(Button button) const;

  /**
   * Query whether any button was pressed this frame.
   */
  bool wasAnyPressed() const;

  /**
   * Query whether any button was released this frame.
   */
  bool wasAnyReleased() const;

  /**
   * Get duration (in milliseconds) that any button has been continuously held.
   */
  unsigned long getHeldTime() const;

  /**
   * Get UI labels for front buttons after remapping.
   * 
   * Maps input labels (back, confirm, previous, next) to physical button positions
   * based on current CrossPointSettings::FRONT_BUTTON_LAYOUT.
   */
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;

  /**
   * Get the physical GPIO index of the front button pressed this frame.
   * 
   * @return Index 0-3 for front buttons, or -1 if no front button was pressed
   */
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;
  mutable bool suppressConfirmReleaseUntilButtonUp = false;

  /**
   * Internal helper: Map a logical button to GPIO state.
   * Handles button layout remapping via CrossPointSettings.
   */
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
