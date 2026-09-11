/*
 * Tikk liquid tilt display
 *
 * LIS2DH tilt drives a top-down "pool" of liquid on the 9x16 IS31FL3731
 * LED matrix. The wave height field is pulled toward a tilt-defined
 * equilibrium plane, so the water pools toward the low side, sloshes on
 * fast tilts, and settles calm when the board is flat (with a gentle
 * ambient shimmer — binary displays do sparkle, not brightness).
 *
 * Single-threaded by design (micro-ROS publisher thread runs separately).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

#include "tikk_led_matrix.h"

#ifdef CONFIG_MICROROS
#include <time.h>
#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <rmw_microros/rmw_microros.h>
#include <microros_transports.h>
#include <geometry_msgs/msg/vector3_stamped.h>

#define MICROROS_PUBLISH_MS 100 /* 10 Hz */
#define MICROROS_THREAD_STACK 24576
#define MICROROS_THREAD_PRIO 5
#endif

#define W 16
#define H 9

#define TICK_MS 50 /* 20 Hz display/sim rate */

/* Liquid tuning */
#define WAVE_K 0.00f      /* propagation stiffness — 0: no traveling waves */
#define WAVE_DAMP 0.80f   /* velocity RETENTION per tick — lower = settles
			  * faster, no ringing (0.97 rang for ~2s) */
#define PLANE_PULL 0.14f  /* how hard water is pulled toward the tilted plane */
#define PLANE_SCALE 0.35f /* full 1g tilt => this much height at display edge */
#define SURF_THRESH 0.15f /* LED on when water depth above this */
#define WATER_LEVEL 0.20f /* flat tray: film depth at rest (water everywhere) */
#define WALL_CLING 0.45f  /* meniscus: how strongly water hugs edges/corners */
#define SHIMMER_PER_TICK 0   /* off: shimmer was a constant agitation source */
#define SHIMMER_AMP 0.04f    /* tiny — just enough to glimmer at rest */

/* Orientation mapping — flip signs at bring-up if water pools the wrong way */
#define AX_SIGN (-1.0f)
#define AY_SIGN (-1.0f)

/* Tilt low-pass: raw accel noise would jiggle the equilibrium plane and
 * keep the water perpetually agitated. Filter it. */
#define TILT_LP 0.2f

static float h[H][W];
static float v[H][W];

static const struct device *const accel = DEVICE_DT_GET_ANY(st_lis2dh);
static const struct device *const leds = DEVICE_DT_GET_ANY(issi_is31fl3731);

/* Latest filtered accel sample, shared with the micro-ROS publisher thread.
 * Torn reads are harmless for telemetry (values are low-passed already). */
static struct {
	float x;
	float y;
	float z;
} latest_accel;

#ifdef CONFIG_MICROROS
static rcl_publisher_t tilt_pub;
static geometry_msgs__msg__Vector3Stamped tilt_msg;

static void tilt_timer_callback(rcl_timer_t *timer, int64_t last_call_time)
{
	RCLC_UNUSED(last_call_time);
	if (timer == NULL) {
		return;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	tilt_msg.header.stamp.sec = ts.tv_sec;
	tilt_msg.header.stamp.nanosec = ts.tv_nsec;
	tilt_msg.vector.x = latest_accel.x;
	tilt_msg.vector.y = latest_accel.y;
	tilt_msg.vector.z = latest_accel.z;

	rcl_publish(&tilt_pub, &tilt_msg, NULL);
}

#define RCCHECK(fn)                                                                        \
	{                                                                                  \
		rcl_ret_t rc = fn;                                                         \
		if (rc != RCL_RET_OK) {                                                    \
			printk("micro-ROS error %d at line %d\n", (int)rc, __LINE__);      \
			return;                                                            \
		}                                                                          \
	}

static void microros_thread(void)
{
	zephyr_transport_params_t transport_params = {0};

	printk("micro-ROS: connecting (plug in agent / open serial port)\n");

	rmw_uros_set_custom_transport(
		MICRO_ROS_FRAMING_REQUIRED,
		(void *)&transport_params,
		zephyr_transport_open,
		zephyr_transport_close,
		zephyr_transport_write,
		zephyr_transport_read);

	rcl_allocator_t allocator = rcl_get_default_allocator();
	rclc_support_t support;
	RCCHECK(rclc_support_init(&support, 0, NULL, &allocator));

	rcl_node_t node;
	RCCHECK(rclc_node_init_default(&node, "tikk_tilt", "", &support));

	RCCHECK(rclc_publisher_init_default(
		&tilt_pub,
		&node,
		ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Vector3Stamped),
		"tilt"));

	rcl_timer_t timer;
	RCCHECK(rclc_timer_init_default(&timer, &support,
					RCL_MS_TO_NS(MICROROS_PUBLISH_MS),
					tilt_timer_callback));

	rclc_executor_t executor;
	RCCHECK(rclc_executor_init(&executor, &support.context, 1, &allocator));
	RCCHECK(rclc_executor_add_timer(&executor, &timer));

	printk("micro-ROS: publishing /tilt at %d ms\n", MICROROS_PUBLISH_MS);

	while (true) {
		rclc_executor_spin_some(&executor, RCL_MS_TO_NS(100));
		k_msleep(10);
	}
}

K_THREAD_DEFINE(microros_tid, MICROROS_THREAD_STACK,
		microros_thread, NULL, NULL, NULL,
		MICROROS_THREAD_PRIO, 0, 0);
#endif /* CONFIG_MICROROS */

static inline float clampf(float val, float lo, float hi)
{
	return val < lo ? lo : (val > hi ? hi : val);
}

/* cheap xorshift for ambient shimmer */
static uint32_t rng_state = 0x1234abcd;
static inline uint32_t rng_next(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void ripple_at(int x, int y, float amp)
{
	if (x < 0 || x >= W || y < 0 || y >= H) {
		return;
	}
	v[y][x] -= amp;
}

static void wave_step(float gx, float gy)
{
	/* velocity update: Laplacian propagation + pull toward the "bowl +
	 * tilted plane" equilibrium (concave bowl: water pools center at rest,
	 * tilt plane drags the pool toward the low side) */
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float n = 0.0f;
			int c = 0;

			if (x > 0) { n += h[y][x - 1]; c++; }
			if (x < W - 1) { n += h[y][x + 1]; c++; }
			if (y > 0) { n += h[y - 1][x]; c++; }
			if (y < H - 1) { n += h[y + 1][x]; c++; }

			float lap = (n / (float)c) - h[y][x];
			float dx = x - (W - 1) / 2.0f;
			float dy = y - (H - 1) / 2.0f;

			/* flat tray of water, tilted by gravity: film covers
			 * everything at rest, drains toward the low side on tilt */
			float eq = WATER_LEVEL + PLANE_SCALE * (gx * dx + gy * dy);

			/* meniscus: water clings to tray walls — distance to
			 * nearest display edge; bonus decays with depth into
			 * the tray. This keeps water in corners past the flat
			 * waterline and makes the corner waterline concave. */
			int d = x;
			if (W - 1 - x < d) { d = W - 1 - x; }
			if (y < d) { d = y; }
			if (H - 1 - y < d) { d = H - 1 - y; }
			eq += WALL_CLING / (1.0f + (float)d);

			if (eq < 0.0f) {
				eq = 0.0f;
			}
			v[y][x] = (v[y][x] + WAVE_K * lap + PLANE_PULL * (eq - h[y][x]))
				  * WAVE_DAMP;
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
	double az = sensor_value_to_double(&a[2]);

	latest_accel.x = (float)ax;
	latest_accel.y = (float)ay;
	latest_accel.z = (float)az;

	/* tilt direction -> slope of the equilibrium water plane (filtered:
	 * raw sensor noise never stops agitating the water) */
	static float fgx, fgy;
	float gx = clampf(AX_SIGN * (float)(ax / 9.81), -1.0f, 1.0f);
	float gy = clampf(AY_SIGN * (float)(ay / 9.81), -1.0f, 1.0f);
	fgx += (gx - fgx) * TILT_LP;
	fgy += (gy - fgy) * TILT_LP;

	wave_step(fgx, fgy);
	render();

	if ((ticks % 100) == 0) {
		printk("tick %u: gx=%.2f gy=%.2f ax=%.2f ay=%.2f\n",
		       ticks, gx, gy, ax, ay);
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
