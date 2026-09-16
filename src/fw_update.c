/*
 * Copyright (c) 2026 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Golioth OTA over pouch: receives the manifest + image, writes to the
 * mcuboot secondary slot, requests the swap, reboots. Slimmed from
 * tikk-fleet/app/tikk-pouch-demo/src/fw_update.c (LED progress calls
 * removed — log only).
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(fw_update, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include <golioth/ota.h>

#include <app_version.h>

#include <stdio.h>

#include <zephyr/device.h>

#include "fw_update.h"
#include "tikk_led_matrix.h"

/* FW_VERSION_STRING lives in fw_update.h — shared with main.c, which
 * scrolls it on the LED matrix at boot. */

/* Set while image blocks are being written: the liquid sim and the
 * micro-ROS publishers stand down (fw_downloading, see main.c), and the
 * LED matrix shows the download progress bar (check_progress). */
volatile bool fw_downloading;

static const struct device *const leds = DEVICE_DT_GET_ANY(issi_is31fl3731);

/* OTA text brightness: brighter than the liquid display (10) so the
 * update is obvious on camera during the demo. */
#define OTA_TEXT_BRIGHTNESS 50

/* Last % shown on the bar; file-scope so it resets cleanly per download. */
static uint8_t ota_last_pct = 255;

static uint32_t fw_size = 1;
static struct flash_img_context flash_context;
static bool download_started;

static void check_progress(uint32_t offset)
{
    uint8_t pct = (offset * 100U) / fw_size;
    if (pct > 100)
    {
        pct = 100;
    }

    if (pct != ota_last_pct)
    {
        LOG_INF("OTA progress: %u%%", pct);
        /* The % AS TEXT on the LED matrix — digits ticking up (" 5%",
         * "50%", "99%") is the demo's "watch it download" moment. The
         * %-glyph's rightmost columns clip off the 15-column display
         * (known geometry — matches the tikk-fleet demo's look).
         * Non-blocking. */
        char pct_str[8];
        snprintf(pct_str, sizeof(pct_str), "%2u%%", pct);
        display_text(leds, pct_str, OTA_TEXT_BRIGHTNESS);
        ota_last_pct = pct;
    }
}

static void ota_main_receive(const void *data, size_t offset, size_t len, bool is_last)
{
    LOG_DBG("Received %d bytes at offset %d", len, offset);

    int err = 0;
    if (!download_started)
    {
        download_started = true;
        fw_downloading = true;
        ota_last_pct = 255;
        display_text(leds, " 0%", OTA_TEXT_BRIGHTNESS);
        LOG_INF("Firmware download started");
    }
    if (0 == offset)
    {
        err = flash_img_init(&flash_context);
        if (err)
        {
            LOG_ERR("Failed to init flash write");
            return;
        }
    }

    check_progress((uint32_t)offset);

    err = flash_img_buffered_write(&flash_context, data, len, is_last);
    if (err)
    {
        LOG_ERR("Failed to write to flash: %d", err);
        return;
    }

    if (is_last)
    {
        fw_downloading = false;
        LOG_INF("Image written; rebooting to apply upgrade");

        err = boot_request_upgrade(BOOT_SWAP_TYPE_TEST);
        if (err)
        {
            LOG_ERR("Failed to request upgrade");
            return;
        }

#if IS_ENABLED(CONFIG_LOG)
        while (log_process())
        {
        }
#endif
        /* Checkmark flourish on the matrix (1 s, self-clears) */
        display_sent_pattern(leds, OTA_TEXT_BRIGHTNESS);
        k_sleep(K_SECONDS(1));
        sys_reboot(SYS_REBOOT_WARM);
    }
}

static void ota_manifest_receive(const struct golioth_ota_manifest_component *components,
                                 size_t num_components)
{
    for (int i = 0; i < num_components; i++)
    {
        fw_size = components[i].size;
        LOG_INF("OTA manifest: %s@%s, %u bytes (running " FW_VERSION_STRING ")",
                components[i].name, components[i].target, fw_size);
        if (0 != strcmp(components[i].current, components[i].target))
        {
            LOG_INF("Marking %s for download", components[i].name);
            golioth_ota_mark_for_download(components[i].name);
        }
        else
        {
            LOG_INF("Already running target version — no update needed");
        }
    }

    download_started = false;
}

GOLIOTH_OTA_COMPONENT(main, "main", FW_VERSION_STRING, ota_main_receive);
GOLIOTH_OTA_MANIFEST_HANDLER(ota_manifest_receive);
