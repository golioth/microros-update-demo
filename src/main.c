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

#include "fw_update.h"

#ifdef CONFIG_MICROROS
#include <version.h>
#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 4, 0)
/* Zephyr 4.4 renamed <zephyr/posix/time.h> -> <zephyr/posix/posix_time.h>.
 * picolibc's <time.h> (SDK 1.0.1) exposes clock_gettime + CLOCK_REALTIME
 * under its default visibility; the implementation lives in
 * src/posix_clock.c (see prj.conf: no POSIX header routing on 4.4). */
#include <time.h>
#else
#include <zephyr/posix/time.h> /* clock_gettime + CLOCK_REALTIME (picolibc's
				* <time.h> hides POSIX names without
				* _POSIX_C_SOURCE, which we no longer set) */
#endif
#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <rmw_microros/rmw_microros.h>
#include <microros_transports.h>
#include <geometry_msgs/msg/vector3_stamped.h>
#include <std_msgs/msg/float32.h>

#define MICROROS_PUBLISH_MS 100 /* 10 Hz */
#define MICROROS_THREAD_STACK 24576
#define MICROROS_THREAD_PRIO 5
#endif

/* Sim grid = the PHYSICAL display: 7x15 of the IS31FL3731's 9x16 matrix
 * is populated (rows 0-6, cols 0-14). Simulating the full 9x16 put water
 * and meniscus in invisible regions. */
#define W 15
#define H 7

#define TICK_MS 50 /* 20 Hz display/sim rate */

/* Liquid tuning */
#define WAVE_K 0.00f      /* propagation stiffness — 0: no traveling waves */
#define WAVE_DAMP 0.80f   /* velocity RETENTION per tick — lower = settles
			  * faster, no ringing (0.97 rang for ~2s) */
#define PLANE_PULL 0.18f  /* how hard water is pulled toward the tilted plane */
#define PLANE_SCALE 0.35f /* floor tilt: full 1g => this much drop per cell */
#define SURF_THRESH 0.15f /* LED on when water depth above this */
#define WATER_LEVEL 0.13f /* TOTAL water volume = this x cell count. Flat =>
			  * uniform film everywhere; long side => ~2-row
			  * pool at the low edge (volume-conserving fill) */
#define WALL_CLING 0.35f  /* meniscus: how strongly water hugs edges/corners */
#define SHIMMER_PER_TICK 0   /* off: shimmer was a constant agitation source */
#define SHIMMER_AMP 0.04f    /* tiny — just enough to glimmer at rest */

/* Orientation mapping — flip signs at bring-up if water pools the wrong way */
#define AX_SIGN (-1.0f)
#define AY_SIGN (-1.0f)

/* Tilt low-pass: raw accel noise would jiggle the equilibrium plane and
 * keep the water perpetually agitated. Filter it. */
#define TILT_LP 0.3f

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

#ifdef CONFIG_TIKK_TEMP_PUBLISHER
/* TMP102 on the Tikk add-on (0x48, ti,tmp112 driver) — the OTA "after"
 * payload: readings published on /temp at 1 Hz. */
static const struct device *const tmp102 = DEVICE_DT_GET_ANY(ti_tmp112);
static rcl_publisher_t temp_pub;
static std_msgs__msg__Float32 temp_msg;
static uint32_t temp_div;
static bool temp_announced;

/* One-shot diagnostics — a permanently failing temp path must not be
 * silent (cost an hour of head-scratching once already). */
#define TEMP_ERR(msg)                                    \
	do {                                             \
		if (!temp_err_shown) {                   \
			printk("temp: " msg "\n");       \
			temp_err_shown = true;           \
		}                                        \
	} while (0)
static bool temp_err_shown;

static void publish_temp(void)
{
	struct sensor_value val;

	if (tmp102 == NULL || !device_is_ready(tmp102)) {
		TEMP_ERR("TMP102 not ready");
		return;
	}
	if (sensor_sample_fetch(tmp102) != 0) {
		TEMP_ERR("TMP102 sample fetch failed");
		return;
	}
	/* tmp112 driver exposes AMBIENT_TEMP (not DIE_TEMP) */
	if (sensor_channel_get(tmp102, SENSOR_CHAN_AMBIENT_TEMP, &val) != 0) {
		TEMP_ERR("TMP102 channel get failed");
		return;
	}
	temp_msg.data = (float)sensor_value_to_double(&val);
	rcl_publish(&temp_pub, &temp_msg, NULL);
	if (!temp_announced) {
		printk("TMP102 online: %.2f C\n", (double)temp_msg.data);
		temp_announced = true;
	}
}
#endif /* CONFIG_TIKK_TEMP_PUBLISHER */

static void tilt_timer_callback(rcl_timer_t *timer, int64_t last_call_time)
{
	RCLC_UNUSED(last_call_time);
	if (timer == NULL) {
		return;
	}

	/* While an OTA image downloads, fw_update owns the LED matrix
	 * (progress bar) and publishing stands down — CPU for the flash
	 * writes, plus a cleaner demo story. The timer keeps firing, so
	 * /tilt + /temp resume on their own when the download ends. */
	if (fw_downloading) {
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

#ifdef CONFIG_TIKK_TEMP_PUBLISHER
	/* /temp at 1 Hz (every 10th 10 Hz tick) */
	if (++temp_div >= 10) {
		temp_div = 0;
		publish_temp();
	}
#endif
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

#ifdef CONFIG_TIKK_TEMP_PUBLISHER
	RCCHECK(rclc_publisher_init_default(
		&temp_pub,
		&node,
		ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
		"temp"));
#endif

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

/* tray floor heights + scratch buffer for the per-tick water-fill */
static float floors[H][W];
static float sorted_floors[H * W];

static void wave_step(float gx, float gy)
{
	/* Volume-conserving liquid: the tray floor tilts with gravity and a
	 * FIXED volume of water fills the lowest cells first (water-fill).
	 * Flat => uniform film covering the display; on its side => the same
	 * water pools at the low edge — pool size is set by WATER_LEVEL
	 * (total volume), not by tilt amount. */
	const float volume = (float)(W * H) * WATER_LEVEL;
	int count = 0;

	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float dx = x - (W - 1) / 2.0f;
			float dy = y - (H - 1) / 2.0f;
			floors[y][x] = -PLANE_SCALE * (gx * dx + gy * dy);
			sorted_floors[count++] = floors[y][x];
		}
	}

	/* insertion sort ascending (105 cells at 20 Hz — trivial) */
	for (int i = 1; i < count; i++) {
		float key = sorted_floors[i];
		int j = i - 1;
		while (j >= 0 && sorted_floors[j] > key) {
			sorted_floors[j + 1] = sorted_floors[j];
			j--;
		}
		sorted_floors[j + 1] = key;
	}

	/* water-fill: raise the surface over the lowest cells until the fixed
	 * volume is used up; everything above the level stays dry */
	float cum = sorted_floors[0];
	float level = volume + cum;
	int k = 0;
	while (k < count - 1 && level > sorted_floors[k + 1]) {
		k++;
		cum += sorted_floors[k];
		level = (volume + cum) / (float)(k + 1);
	}

	/* relax toward (fill depth + meniscus), with momentum for slosh */
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float neigh = 0.0f;
			int c = 0;

			if (x > 0) { neigh += h[y][x - 1]; c++; }
			if (x < W - 1) { neigh += h[y][x + 1]; c++; }
			if (y > 0) { neigh += h[y - 1][x]; c++; }
			if (y < H - 1) { neigh += h[y + 1][x]; c++; }

			float lap = (neigh / (float)c) - h[y][x];

			float depth = level - floors[y][x];
			if (depth < 0.0f) {
				depth = 0.0f;
			}

			/* meniscus: water clings to tray walls — but only where
			 * water exists. Gated by the cell's own depth so dry
			 * border cells don't light as a phantom frame (ungated
			 * cling put a permanent 2-LED border around the display).
			 * At the waterline the gate ramps 0..1 over 0.10 depth,
			 * giving the concave corner climb where water is. */
			int d = x;
			if (W - 1 - x < d) { d = W - 1 - x; }
			if (y < d) { d = y; }
			if (H - 1 - y < d) { d = H - 1 - y; }
			float gate = depth * 10.0f;
			if (gate > 1.0f) {
				gate = 1.0f;
			}
			float eq = depth + gate * WALL_CLING / (1.0f + (float)d);

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
	/* full 9-row controller buffer; physical rows 7-8 left off */
	uint16_t fb9[9] = {0};

	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			if (h[y][x] > SURF_THRESH) {
				fb9[y] |= (uint16_t)(1 << x);
			}
		}
	}

	display_framebuffer(leds, fb9);
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

	/* Firmware version on the matrix at boot — the same "vX.Y.Z"
	 * scroll the tikk-fleet demo showed (led_message("v" ...) there).
	 * FW_VERSION_STRING is the OTA identity reported to the cloud, so
	 * the boot screen says which half of the demo pair is running
	 * (0.1.1 before / 0.2.1 after). Blocking ~3 s, like the fleet demo. */
	scroll_message(leds, "v" FW_VERSION_STRING, 40);

	while (true) {
		/* fw_update owns the LED matrix during an OTA download —
		 * stand the liquid sim down until it finishes. */
		if (fw_downloading) {
			k_msleep(TICK_MS);
			continue;
		}
		if (tick() < 0) {
			printk("sensor fetch failed\n");
		}
		k_msleep(TICK_MS);
	}

	return 0;
}
