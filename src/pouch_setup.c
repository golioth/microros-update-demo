/*
 * Copyright (c) 2026 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pouch (over BLE GATT) + Golioth bring-up for the Tikk micro-ROS demo.
 * Runs in its own thread so the liquid display + micro-ROS publisher are
 * unaffected. Pattern from tikk-fleet/app/tikk-pouch-demo/src/main.c.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pouch_setup, LOG_LEVEL_INF);

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/drivers/gpio.h>

#include <pouch/pouch.h>
#include <pouch/events.h>
#include <pouch/transport/ble_gatt/peripheral.h>
#include <pouch/transport/ble_gatt/common/types.h>

#include "credentials.h"

#include <app_version.h>

#define POUCH_THREAD_STACK 4096
#define POUCH_THREAD_PRIO 5

static struct
{
    uint8_t uuid[16];
    struct golioth_ble_gatt_adv_data data;
} __packed service_data = {
    .uuid = {GOLIOTH_BLE_GATT_UUID_SVC_VAL},
    .data =
        {
            .version = (POUCH_VERSION << GOLIOTH_BLE_GATT_ADV_VERSION_POUCH_SHIFT)
                | (GOLIOTH_BLE_GATT_VERSION << GOLIOTH_BLE_GATT_ADV_VERSION_SELF_SHIFT),
            .flags = 0x0,
        },
};

static struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static struct bt_data sd[] = {
    BT_DATA(BT_DATA_SVC_DATA128, &service_data, sizeof(service_data)),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_INF("BLE connection failed (err 0x%02x)", err);
    }
    else
    {
        LOG_INF("BLE connected (gateway)");
    }
}

static void disconnect_work_handler(struct k_work *work)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err)
    {
        LOG_ERR("Advertising failed to start (err %d)", err);
    }
}

K_WORK_DELAYABLE_DEFINE(disconnect_work, disconnect_work_handler);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE disconnected (reason 0x%02x) — re-advertising", reason);

    k_work_schedule(&disconnect_work, K_SECONDS(1));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* Sync-request button (P0.11 on the Tikk add-on): pressing it sets the
 * sync-request flag in the BLE advertisement — a scanning gateway sees
 * the flag, connects, and a pouch session syncs to the cloud. The flag
 * clears itself when the session ends. Demo trigger: deliberate, visible,
 * testable. (A periodic timer can re-arm this later.) */
static const struct gpio_dt_spec sync_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback sync_button_cb;

static void request_sync_work_handler(struct k_work *work)
{
    int err;

    service_data.data.flags |= GOLIOTH_BLE_GATT_ADV_FLAG_SYNC_REQUEST;
    err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err)
    {
        LOG_ERR("Failed to update advertising data (err %d)", err);
        return;
    }
    LOG_INF("Sync requested — flag set, waiting for a gateway to connect");
}
K_WORK_DEFINE(request_sync_work, request_sync_work_handler);

static void sync_button_isr(const struct device *dev,
                             struct gpio_callback *cb, uint32_t pins)
{
    k_work_submit(&request_sync_work);
}

static int init_sync_button(void)
{
    if (!gpio_is_ready_dt(&sync_button))
    {
        LOG_WRN("Sync button not ready (P0.11)");
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(&sync_button, GPIO_INPUT);
    if (err)
    {
        return err;
    }

    gpio_init_callback(&sync_button_cb, sync_button_isr, BIT(sync_button.pin));
    err = gpio_add_callback_dt(&sync_button, &sync_button_cb);
    if (err)
    {
        return err;
    }

    return gpio_pin_interrupt_configure_dt(&sync_button, GPIO_INT_EDGE_TO_ACTIVE);
}

static void pouch_event_handler(enum pouch_event event, void *ctx)
{
    switch (event)
    {
    case POUCH_EVENT_SESSION_START:
        LOG_INF("Pouch: session started (gateway connected)");
        break;
    case POUCH_EVENT_SESSION_END:
        /* Session complete — clear the sync-request flag until the next
         * button press. */
        service_data.data.flags = 0;
        bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
        LOG_INF("Pouch: session ended");
        break;
    default:
        break;
    }
}

POUCH_EVENT_HANDLER(pouch_event_handler, NULL);

static void pouch_thread(void)
{
    /* Let the system settle (USB up, display running) before bringing up
     * the BT controller — the SoftDevice Controller grabs radio IRQs. */
    k_sleep(K_SECONDS(8));

    LOG_INF("Pouch SDK version: " STRINGIFY(APP_BUILD_VERSION));
    LOG_INF("Pouch protocol version: %d, BLE transport version: %d",
            POUCH_VERSION, GOLIOTH_BLE_GATT_VERSION);

    int err = golioth_ble_gatt_peripheral_init();
    if (err)
    {
        LOG_ERR("Failed to initialize Pouch BLE GATT peripheral (err %d)", err);
        return;
    }

    err = bt_enable(NULL);
    if (err)
    {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return;
    }
    LOG_INF("Bluetooth initialized");

    struct pouch_config config = {0};

    err = load_certificate(&config.certificate);
    if (err)
    {
        LOG_WRN("No device certificate — run provisioning (see README)");
        return;
    }

    config.private_key = load_private_key();
    if (config.private_key == PSA_KEY_ID_NULL)
    {
        LOG_WRN("No device private key — run provisioning (see README)");
        free_certificate(&config.certificate);
        return;
    }

    LOG_INF("Credentials loaded");

    err = pouch_init(&config);
    if (err)
    {
        LOG_ERR("Pouch init failed (err %d)", err);
        free_certificate(&config.certificate);
        return;
    }
    free_certificate(&config.certificate);

    LOG_INF("Pouch initialized");

    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err)
    {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return;
    }

    LOG_INF("Advertising as \"%s\" — waiting for a gateway", CONFIG_BT_DEVICE_NAME);

    init_sync_button();
    LOG_INF("Sync button ready (P0.11) — press to request a sync");
}

K_THREAD_DEFINE(pouch_tid, POUCH_THREAD_STACK,
                pouch_thread, NULL, NULL, NULL,
                POUCH_THREAD_PRIO, 0, 0);
