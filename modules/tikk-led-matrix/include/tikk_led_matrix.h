/*
 * Copyright (c) 2025 Golioth
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

/**
 * @brief Clear the display
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 */
void display_clear(const struct device *dev);

/**
 * @brief Write a full 9-row x 16-column framebuffer to the display
 *
 * Double-buffered: writes to a background frame, then flips the display to
 * it. Frame brightness is whatever was set previously (display_clear sets 20%).
 *
 * - The zero-eth row of the framebuffer is y=0
 * - The zero-eth bit of the row is x=0
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param fb Array of nine 16-bit values representing on(1)/off(0) pixel values
 */
void display_framebuffer(const struct device *dev, uint16_t *fb);

/**
 * @brief Render a short static text string on the display (5x8 font)
 *
 * Non-blocking: renders glyphs left-to-right into the back frame and
 * flips to it, then returns (unlike scroll_message, which blocks until
 * the scroll finishes). Text wider than the 15-column display is
 * CLIPPED on the right — ~2.5 glyphs fit; the last glyph may be
 * partially cut off (the font's 8th row also clips on the physical
 * 7-row matrix).
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param text Null-terminated string; chars outside 32..126 render as space
 * @param brightness Brightness percentage [0..100]
 */
void display_text(const struct device *dev, const char *text, uint8_t brightness);

/**
 * @brief Display animated scrolling arrows
 *
 * Show a scrolling chevron (arrow) pattern on the Tikk display. This function uses the animation
 * feature of the driver chip. The function will return when the frames of the aniation are written
 * to the driver and the animation is started. The driver chip will continue running the animation
 * after program continues.
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param wide If true, arrows point the wide side of the display, otherwise the long side
 * @param reverse If true, scrolling direction is reversed
 */
void scroll_arrows(const struct device *dev, bool wide, bool reverse);

/**
 * @brief Scroll a message on the display using 5x7 characters
 *
 * Screen will start empty, then scroll text from right to left until the display is empty. This is
 * a blocking function that will not return until the scrolling message has finished.
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param message Null-terminated string to scroll on the display
 * @param delay_ms Milliseconds to delay before shifting left one column
 */
void scroll_message(const struct device *dev, const char *message, uint32_t delay_ms);

/**
 * @brief Scroll a string of hexadecimal characters using 3x5 font
 *
 * Scrolls a message made up of `0` through `F` characters (`x` is also available). This is a
 * blocking function that will not return until the scrolling message has finished.
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param message Null-terminated string to scroll on the display
 * @param delay_ms Milliseconds to delay before shifting left one column
 */
void scroll_hex(const struct device *dev, const char *message, uint32_t delay_ms);

/**
 * @brief Display last four chars of a string of hexadecimal characters using 3x5 font
 *
 * Display the last four characters of a hex string. This function will update the display and
 * return. The display will continue to show the hex characters until new commands are sent to it.
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param message Null-terminated string to scroll on the display
 */
void display_hex_message(const struct device *dev, const char *message);

/**
 * @brief Quikly cycle a message one letter at a time at the center of the display
 *
 * The displayed MESSAGE is #define at the top of tikk_led_matrix.c This is a blocking function that
 * will not return until the scrolling message has finished.
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 */
void show_tikk_banner(const struct device *dev);

/**
 * @brief Display filled columns on the LED matrix
 *
 * Lights up the specified number of columns (from left to right) on the 7x15 display.
 * Used for countdown visualization.
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param columns Number of columns to light up [0..15]
 * @param brightness Brightness percentage [0..100]
 */
void display_countdown_columns(const struct device *dev, uint8_t columns, uint8_t brightness);

/**
 * @brief Flash a success indicator on the display for one second
 *
 * Displays a checkmark/confirmation pattern on the 7x15 LED matrix at low brightness
 * for 1 second, then clears the display.
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param brightness Brightness percentage [0..100]
 */
void display_sent_pattern(const struct device *dev, uint8_t brightness);
