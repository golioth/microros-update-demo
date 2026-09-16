/*
 * Copyright (c) 2026 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared OTA state. fw_update.c owns the LED matrix while an image
 * download is in flight (progress bar + completion checkmark), and the
 * liquid sim + micro-ROS publishers stand down until it finishes —
 * CPU headroom for the flash writes, and a cleaner demo story.
 */

#pragma once

#include <stdbool.h>

#include <app_version.h>

/* OTA version identity for the demo pair: the "before" build (temp
 * publisher off) reports 0.1.2 so the Golioth release for 0.2.2 triggers
 * the update; the "after" build reports the VERSION-file string.
 * The same string is shown on the LED matrix at boot ("vX.Y.Z" scroll,
 * like the tikk-fleet demo) so the screen tells you which half of the
 * pair is running. */
#ifdef CONFIG_TIKK_TEMP_PUBLISHER
#define FW_VERSION_STRING APP_VERSION_STRING
#else
#define FW_VERSION_STRING "0.1.2"
#endif

/* True while an OTA image download is being written to the mcuboot
 * secondary slot. Volatile: written from the pouch downlink path,
 * read from the main sim loop and the micro-ROS publish timer. */
extern volatile bool fw_downloading;

/* True between "marked for download" (manifest) and download end.
 * pouch_setup watches it to re-arm the sync flag on BLE disconnect, so
 * a dropped link mid-download retries automatically instead of hanging
 * until a human resets the board. */
extern volatile bool fw_download_pending;