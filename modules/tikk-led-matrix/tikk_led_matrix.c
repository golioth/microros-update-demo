/*
 * Copyright (c) 2025 Golioth
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/led/is31fl3731.h>
#include <zephyr/kernel.h>

#include <font5x8.h>
#include "small_hex_font.h"
#include "reverse_bits.h"
#include "scrolling_arrows.h"

#define BANNER "CANONICAL"

/* Global display brightness (percent). 20 was retina-searing at the bench. */
#define DISPLAY_BRIGHTNESS 10

/**
 * @brief Fill all 18 8-bit channels of pixel data from a 9-row uint16_t framebuffer
 *
 * - The zero-eth row of the framebuffer is y=0
 * - The zero-eth bit of the row is x=0
 *
 * The bit-order is intuitvely backwards but makes sense for a screen where 0,0 is upper-left.
 *
 * @param dev An instance of an is31fl3731 LED matrix driver
 * @param fb Array of nine 16-bit values representing on(1)/off(0) pixel values
 * @param frame_num Frame number of the display to write to [0..7]
 * @param invert_h Flip the display horizontally if true
 * @param invert_v Flip the display vertically if true
 * @return 0 if successful, otherwise a (negative) error code
 */
static int fill_frame_from_buffer(const struct device *dev,
                                  uint16_t *fb,
                                  uint8_t frame_num,
                                  bool invert_h,
                                  bool invert_v)
{
    if (NULL == fb)
    {
        return -ESRCH;
    }

    uint8_t chan_buf[18];
    uint8_t byte_0, byte_1;

    for (int i = 0; i < 9; i++)
    {
        uint8_t fb_idx = (invert_v) ? 8 - i : i;

        if (invert_h)
        {
            byte_0 = reverse_bits[fb[fb_idx] >> 8];
            byte_1 = reverse_bits[fb[fb_idx] & 0xff];
        }
        else
        {
            byte_0 = fb[fb_idx] & 0xff;
            byte_1 = fb[fb_idx] >> 8;
        }

        memset(chan_buf + (i * 2), byte_0, 1);
        memset(chan_buf + (i * 2) + 1, byte_1, 1);
    }

    is31fl3731_select_frame_to_write(dev, frame_num);
    led_write_channels(dev, 0, 18, chan_buf);
    return 0;
}

static void write_font_to_fb(uint16_t *fb, char c, uint8_t offset)
{

    for (int col = 0; col < 5; col++)
    {
        uint8_t slice = font5x8[((c - 32) * 5) + col];

        for (int bit = 0; bit < 8; bit++)
        {
            if (slice & (1 << bit))
            {
                fb[bit] |= (uint16_t) (1 << (offset + col));
            }
        }
    }
}

static int get_hex_idx(char c)
{
    if (('0' <= c) && (c <= '9'))
    {
        return (c - '0');
    }
    else if (('a' <= c) && (c <= 'f'))
    {
        return (c - 'a' + 10);
    }
    else if (('A' <= c) && (c <= 'F'))
    {
        return (c - 'A' + 10);
    }
    else if (' ' == c)
    {
        return HEX_BLANK;
    }

    return HEX_X;
}

static void display_clear_internal(const struct device *dev, int frame, int brightness)
{
    int bright_pct = (100 < brightness) ? 100 : brightness;
    is31fl3731_set_picture_mode(dev);
    is31fl3731_select_frame_to_write(dev, frame);
    is31fl3731_frame_clear(dev);
    is31fl3731_frame_set_brightness(dev, bright_pct);
    is31fl3731_picture_set_display_frame(dev, frame);
}

void display_clear(const struct device *dev)
{
    display_clear_internal(dev, 0, 20);
}

void display_framebuffer(const struct device *dev, uint16_t *fb)
{
    static uint8_t back_frame;

    /* Write to the back frame (not currently displayed), then flip */
    uint8_t write_frame = back_frame ^ 1;

    is31fl3731_set_picture_mode(dev);
    /* Brightness is per-frame — must set it on every frame we write,
     * or the display alternates between two brightness levels each flip */
    is31fl3731_select_frame_to_write(dev, write_frame);
    is31fl3731_frame_set_brightness(dev, DISPLAY_BRIGHTNESS);
    fill_frame_from_buffer(dev, fb, write_frame, false, false);
    is31fl3731_picture_set_display_frame(dev, write_frame);
    back_frame = write_frame;
}

void scroll_hex(const struct device *dev, const char *message, uint32_t delay_ms)
{
    display_clear_internal(dev, 1, 20);

    uint8_t hex_msg[strlen(message)];
    for (int i = 0; i < strlen(message); i++)
    {
        hex_msg[i] = get_hex_idx(message[i]);
    }

    uint16_t fb[18] = {0};

    int w_space = HEX_FONT_W + 1;  // Width of 1 char plus a column for space
    uint32_t limit = (strlen(message) * w_space) + (2 * 16);

    for (uint32_t idx = 0; idx < limit; idx++)
    {
        int font_idx;
        if (idx >= (strlen(message) * w_space))
        {
            font_idx = HEX_BLANK;
        }
        else
        {
            font_idx = hex_msg[idx / w_space];
        }

        uint8_t slice_idx = (idx % w_space);
        uint8_t slice =
            (slice_idx == HEX_FONT_W) ? 0x00 : hex3x5[(font_idx * HEX_FONT_W) + slice_idx];

        for (int bit = 0; bit < HEX_FONT_H; bit++)
        {
            if (slice & (1 << bit))
            {
                fb[bit + 1] |= (uint16_t) (1 << 15);
            }
        }

        fill_frame_from_buffer(dev, fb, 1, false, false);

        for (int i = 0; i < ARRAY_SIZE(fb); i++)
        {
            fb[i] >>= 1;
        }

        k_msleep(delay_ms);
    }
}

static void write_hex_char_to_fb(uint16_t *fb, char c, uint8_t offset)
{
    int char_idx = get_hex_idx(c);

    for (int col = 0; col < HEX_FONT_W; col++)
    {
        uint8_t slice = hex3x5[(char_idx * HEX_FONT_W) + col];

        for (int bit = 0; bit < HEX_FONT_H; bit++)
        {
            if (slice & (1 << (bit)))
            {
                fb[bit + 1] |= (uint16_t) (1 << (offset + col));
            }
        }
    }
}

void display_hex_message(const struct device *dev, const char *message)
{
    display_clear_internal(dev, 0, 20);

    uint16_t fb[18] = {0};

    int start_idx = 0;
    int msg_len = 4;
    if (msg_len <= strlen(message))
    {
        start_idx = strlen(message) - 4;
    }
    else
    {
        msg_len = strlen(message);
    }

    for (int i = 0; i < msg_len; i++)
    {
        write_hex_char_to_fb(fb, message[i + start_idx], (i * (HEX_FONT_W + 1)));
    }

    fill_frame_from_buffer(dev, fb, 0, false, false);
}

void scroll_message(const struct device *dev, const char *message, uint32_t delay_ms)
{
    display_clear_internal(dev, 1, 20);

    uint16_t fb[18] = {0};

    uint32_t limit = (strlen(message) * 6) + (2 * 16);

    for (uint32_t idx = 0; idx < limit; idx++)
    {
        char c = (idx >= (strlen(message) * 6)) ? ' ' : message[idx / 6];
        uint8_t slice_idx = (idx % 6);
        uint8_t slice = (slice_idx == 5) ? 0x00 : font5x8[((c - 32) * 5) + slice_idx];

        for (int bit = 0; bit < 8; bit++)
        {
            if (slice & (1 << bit))
            {
                fb[bit] |= (uint16_t) (1 << 15);
            }
        }

        fill_frame_from_buffer(dev, fb, 1, false, false);

        for (int i = 0; i < ARRAY_SIZE(fb); i++)
        {
            fb[i] >>= 1;
        }

        k_msleep(delay_ms);
    }
}

void scroll_arrows(const struct device *dev, bool wide, bool reverse)
{
    uint16_t (*font)[9] = (true == wide) ? arrow_wide : arrow_narrow;
    uint16_t chan_buf[9];

    for (uint8_t i = 0; i < 5; i++)
    {
        for (int r = 0; r < 9; r++)
        {
            if (true == reverse)
            {
                chan_buf[r] = font[i][8 - r];
            }
            else
            {
                if (true == wide)
                {
                    chan_buf[r] = font[i][r];
                }
                else
                {
                    uint8_t hi_byte = reverse_bits[(uint8_t) font[i][(r + 2) % 9]];
                    uint8_t lo_byte = reverse_bits[font[i][(r + 2) % 9] >> 8];
                    chan_buf[r] = ((uint16_t) hi_byte) << 8 | lo_byte;
                }
            }
        }
        is31fl3731_select_frame_to_write(dev, i + 1);
        is31fl3731_frame_clear(dev);
        is31fl3731_frame_set_brightness(dev, 20);
        led_write_channels(dev, 0, 18, (uint8_t *) chan_buf);
    }

    is31fl3731_set_frame_delay(dev, 9);
    is31fl3731_autoplay_set_frame(dev, 5, 0);
    is31fl3731_set_autoplay_mode(dev, 1);
}

void show_tikk_banner(const struct device *dev)
{
    display_clear_internal(dev, 0, 100);

    int16_t fb[9] = {0};

    for (int i = 0; i < strlen(BANNER); i++)
    {
        for (int row = 0; row < 9; row++)
        {
            fb[row] = 0x0000;
        }
        write_font_to_fb(fb, BANNER[i], 5);
        fill_frame_from_buffer(dev, fb, 0, false, false);

        k_msleep(140);
    }
}

void display_countdown_columns(const struct device *dev, uint8_t columns, uint8_t brightness)
{
    static uint8_t back_frame;

    if (columns > 15)
    {
        columns = 15;
    }

    /* Write to the back frame (not currently displayed) */
    uint8_t write_frame = back_frame ^ 1;

    is31fl3731_set_picture_mode(dev);
    is31fl3731_select_frame_to_write(dev, write_frame);
    is31fl3731_frame_clear(dev);
    is31fl3731_frame_set_brightness(dev, brightness);

    uint16_t fb[9] = {0};
    /* Light up 7 rows (rows 0..6) for the specified number of columns */
    uint16_t col_mask = (columns > 0) ? ((uint16_t)((1 << columns) - 1)) : 0;

    for (int row = 0; row < 7; row++)
    {
        fb[row] = col_mask;
    }

    fill_frame_from_buffer(dev, fb, write_frame, false, false);

    /* Flip to the newly written frame */
    is31fl3731_picture_set_display_frame(dev, write_frame);
    back_frame = write_frame;
}

void display_sent_pattern(const struct device *dev, uint8_t brightness)
{
    static uint8_t back_frame;
    uint8_t write_frame = back_frame ^ 1;

    /* Checkmark pattern, 7 rows x 15 cols, bit 0 = leftmost column */
    static const uint16_t pattern[7] = {
        0x01C0, /* 000000111000000 */
        0x0FF8, /* 000111111111000 */
        0x0FF8, /* 000111111111000 */
        0x03E0, /* 000001111100000 */
        0x03E0, /* 000001111100000 */
        0x0360, /* 000001101100000 */
        0x0360, /* 000001101100000 */
    };

    uint16_t fb[9] = {0};
    for (int row = 0; row < 7; row++) {
        fb[row] = pattern[row];
    }

    is31fl3731_set_picture_mode(dev);
    is31fl3731_select_frame_to_write(dev, write_frame);
    is31fl3731_frame_clear(dev);
    is31fl3731_frame_set_brightness(dev, brightness);
    fill_frame_from_buffer(dev, fb, write_frame, false, false);
    is31fl3731_picture_set_display_frame(dev, write_frame);
    back_frame = write_frame;

    k_msleep(1000);

    /* Clear */
    write_frame ^= 1;
    is31fl3731_select_frame_to_write(dev, write_frame);
    is31fl3731_frame_clear(dev);
    is31fl3731_picture_set_display_frame(dev, write_frame);
    back_frame = write_frame;
}
