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

/* OTA version identity for the demo pair: the "before" build (temp
 * publisher off) reports 0.1.0 so the Golioth release for 0.2.0 triggers
 * the update; the "after" build reports the VERSION-file string. */
#if IS_ENABLED(CONFIG_TIKK_TEMP_PUBLISHER)
#define FW_VERSION_STRING APP_VERSION_STRING
#else
#define FW_VERSION_STRING "0.1.0"
#endif

static uint32_t fw_size = 1;
static struct flash_img_context flash_context;
static bool download_started;

static void check_progress(uint32_t offset)
{
    static uint8_t last_pct = 255;

    uint8_t pct = (offset * 100U) / fw_size;
    if (pct > 100)
    {
        pct = 100;
    }

    if (pct != last_pct)
    {
        LOG_INF("OTA progress: %u%%", pct);
        last_pct = pct;
    }
}

static void ota_main_receive(const void *data, size_t offset, size_t len, bool is_last)
{
    LOG_DBG("Received %d bytes at offset %d", len, offset);

    int err = 0;
    if (!download_started)
    {
        download_started = true;
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
        err = boot_request_upgrade(BOOT_SWAP_TYPE_TEST);
        if (err)
        {
            LOG_ERR("Failed to request upgrade");
            return;
        }

        LOG_INF("Image written; rebooting to apply upgrade");

#if IS_ENABLED(CONFIG_LOG)
        while (log_process())
        {
        }
#endif
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
