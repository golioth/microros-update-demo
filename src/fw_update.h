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

/* True while an OTA image download is being written to the mcuboot
 * secondary slot. Volatile: written from the pouch downlink path,
 * read from the main sim loop and the micro-ROS publish timer. */
extern volatile bool fw_downloading;