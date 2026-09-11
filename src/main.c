/*
 * Tikk liquid tilt display
 *
 * LIS2DH tilt drives a rolling "ball" on the 9x16 IS31FL3731 LED matrix.
 * The ball excites ripples in a simple height-field wave simulation, so the
 * display surface behaves like a shallow pool of liquid.
 *
 * Single-threaded by design (micro-ROS publisher thread joins later).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

#include "tikk_led_matrix.h"

#define W 16
#define H 9

#define TICK_MS 50 /* 20 Hz display/sim rate */

/* Wave simulation tuning */
#define WAVE_K 0.32f    /* propagation stiffness */
#define WAVE_DAMP 0.90f /* velocity damping per tick */
#define SURF_THRESH 0.06f /* LED on when height above this */
#define RIPPLE_AMP 0.65f  /* dip injected per unit ball movement */
#define AMBIENT_TICKS 60  /* raindrop interval when idle */

/* Ball follows tilt, low-passed */
#define TILT_LP 0.18f

/* Orientation mapping — flip signs/axes at bring-up if the ball moves the
 * wrong way for how the board is held. */
#define AX_SIGN 1.0f
#define AY_SIGN 1.0f

static float h[H][W];
static float v[H][W];

static float ball_x = (W - 1) / 2.0f;
static float ball_y = (H - 1) / 2.0f;

static const struct device *const accel = DEVICE_DT_GET_ANY(st_lis2dh);
static const struct device *const leds = DEVICE_DT_GET_ANY(issi_is31fl3731);

static inline float clampf(float val, float lo, float hi)
{
	return val < lo ? lo : (val > hi ? hi : val);
}

static void ripple_at(int x, int y, float amp)
{
	if (x < 0 || x >= W || y < 0 || y >= H) {
		return;
	}
	v[y][x] -= amp;
}

static void wave_step(void)
{
	/* velocity update from Laplacian of height (using current heights) */
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float n = 0.0f;
			int c = 0;

			if (x > 0) { n += h[y][x - 1]; c++; }
			if (x < W - 1) { n += h[y][x + 1]; c++; }
			if (y > 0) { n += h[y - 1][x]; c++; }
			if (y < H - 1) { n += h[y + 1][x]; c++; }

			float lap = (n / (float)c) - h[y][x];
			v[y][x] = (v[y][x] + WAVE_K * lap) * WAVE_DAMP;
		}
	}

	/* height update */
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			h[y][x] += v[y][x];
		}
	}
}

static void render(void)
{
	uint16_t fb[H] = {0};

	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			if (h[y][x] > SURF_THRESH) {
				fb[y] |= (uint16_t)(1 << x);
			}
		}
	}

	/* ball pixel always on */
	int bx = (int)clampf(ball_x, 0, W - 1);
	int by = (int)clampf(ball_y, 0, H - 1);
	fb[by] |= (uint16_t)(1 << bx);

	display_framebuffer(leds, fb);
}

static int tick(void)
{
	static unsigned int ticks;
	struct sensor_value a[3];

	if (sensor_sample_fetch(accel) < 0) {
		return -EIO;
	}
	if (sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, a) < 0) {
		return -EIO;
	}

	double ax = sensor_value_to_double(&a[0]);
	double ay = sensor_value_to_double(&a[1]);

	/* target position: full 1g tilt puts the ball at the edge */
	float tx = clampf((W - 1) / 2.0f + (AX_SIGN * (float)(ax / 9.81)) * ((W - 1) / 2.0f),
			  0, W - 1);
	float ty = clampf((H - 1) / 2.0f + (AY_SIGN * (float)(ay / 9.81)) * ((H - 1) / 2.0f),
			  0, H - 1);

	float px = ball_x;
	float py = ball_y;
	ball_x += (tx - ball_x) * TILT_LP;
	ball_y += (ty - ball_y) * TILT_LP;

	float dx = ball_x - px;
	float dy = ball_y - py;
	float moved = dx * dx + dy * dy;

	if (moved > 0.0025f) {
		ripple_at((int)clampf(ball_x, 0, W - 1),
			  (int)clampf(ball_y, 0, H - 1),
			  RIPPLE_AMP * clampf(moved * 8.0f, 0.2f, 1.5f));
	} else if ((ticks % AMBIENT_TICKS) == 0) {
		/* idle raindrop at the ball position */
		ripple_at((int)clampf(ball_x, 0, W - 1),
			  (int)clampf(ball_y, 0, H - 1),
			  0.5f);
	}

	wave_step();
	render();

	if ((ticks % 100) == 0) {
		printk("tick %u: ax=%.2f ay=%.2f ball=(%.1f,%.1f)\n",
		       ticks, ax, ay, ball_x, ball_y);
	}
	ticks++;

	return 0;
}

int main(void)
{
	printk("Tikk liquid tilt display\n");

	if ((accel == NULL) || !device_is_ready(accel)) {
		printk("LIS2DH not ready\n");
		return 0;
	}
	if ((leds == NULL) || !device_is_ready(leds)) {
		printk("IS31FL3731 not ready\n");
		return 0;
	}

	display_clear(leds);
	printk("sensors ready, starting sim (%d ms tick)\n", TICK_MS);

	while (true) {
		if (tick() < 0) {
			printk("sensor fetch failed\n");
		}
		k_msleep(TICK_MS);
	}

	return 0;
}
