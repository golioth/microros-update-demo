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
	double az = sensor_value_to_double(&a[2]);

	latest_accel.x = (float)ax;
	latest_accel.y = (float)ay;
	latest_accel.z = (float)az;

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
