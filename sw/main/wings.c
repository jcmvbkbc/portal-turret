#include "driver/mcpwm.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "player.h"
#include "wings.h"

#define GPIO_END_SWITCH		23

#define GPIO_WINGSPAN		32
#define CHANNEL_WINGSPAN	MCPWM0A
#define TIMER_WINGSPAN		MCPWM_TIMER_0

#define WINGSPAN_NEUTRAL	0
#define WINGSPAN_RAMP_TIME	MS_TO_TICKS(2000)
#define WINGSPAN_OPEN_START	1400
#define WINGSPAN_OPEN_END	1200
#define WINGSPAN_CLOSE_START	1500
#define WINGSPAN_CLOSE_END	1600

#define GPIO_WINGTURN		33
#define CHANNEL_WINGTURN	MCPWM1A
#define TIMER_WINGTURN		MCPWM_TIMER_1

#define WINGTURN_CENTER		1500
#define WINGTURN_LEFT		1200
#define WINGTURN_RIGHT		1800

#define WINGTURN_RANGE		128

#define MS_TO_TICKS(ms)		((ms) / 10)
#define TICKS_OPENING		MS_TO_TICKS(1700)
#define TICKS_OPENING_DEAD	MS_TO_TICKS(800)
#define TICKS_CENTERING		MS_TO_TICKS(1000)
#define TICKS_CLOSE_TIMEOUT	MS_TO_TICKS(5000)

enum {
	STATE_INITIAL,
	STATE_OPENING,
	STATE_OPEN,
	STATE_CENTERING,
	STATE_CLOSING,
	STATE_CLOSED,
	STATE_BROKEN,
};

struct wings_struct
{
	int state;
	int target;
	int tick;
	int angle; /* 0 -- leftmost, WINGTURN_RANGE -- rightmost*/
	int scan_direction;
	int last_scan_direction;
};

static struct wings_struct wings;

static void end_switch_init(void)
{
	gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_INPUT,
		.pin_bit_mask = 1 << GPIO_END_SWITCH,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};

	gpio_config(&io_conf);
}

static void servo_init(int gpio, int channel, int timer)
{
	mcpwm_config_t pwm_config = {
		.frequency = 50,
		.cmpr_a = 0,
		.counter_mode = MCPWM_UP_COUNTER,
		.duty_mode = MCPWM_DUTY_MODE_0,
	};

	mcpwm_gpio_init(MCPWM_UNIT_0, channel, gpio);
	mcpwm_init(MCPWM_UNIT_0, timer, &pwm_config);
}

static void servo_set_duty(int timer, int us)
{
	mcpwm_set_duty_in_us(MCPWM_UNIT_0, timer, MCPWM_OPR_A, us);
}

static int interpolate(int t, int dt, int start, int end)
{
	return t < dt ? (start * (dt - t) + end * t) / dt : end;
}

static void wings_set_turn(void)
{
	servo_set_duty(TIMER_WINGTURN,
		       interpolate(wings.angle, WINGTURN_RANGE,
				   WINGTURN_LEFT, WINGTURN_RIGHT));
}

static void wings_broken(void)
{
	wings.state = STATE_BROKEN;
	servo_set_duty(TIMER_WINGSPAN, 0);
	servo_set_duty(TIMER_WINGTURN, 0);
}

static void wings_closing(void)
{
	if (wings_closed()) {
		wings.state = STATE_CLOSED;
	} else if (wings_opened()) {
		wings.state = STATE_CENTERING;
		servo_set_duty(TIMER_WINGSPAN, WINGSPAN_NEUTRAL);
		wings_set_turn();
	} else {
		wings.state = STATE_CLOSING;
		servo_set_duty(TIMER_WINGSPAN, WINGSPAN_CLOSE_START);
		wings.tick = 0;
	}
}

void wings_init(void)
{
	servo_init(GPIO_WINGSPAN, CHANNEL_WINGSPAN, TIMER_WINGSPAN);
	servo_init(GPIO_WINGTURN, CHANNEL_WINGTURN, TIMER_WINGTURN);
	servo_set_duty(TIMER_WINGSPAN, WINGSPAN_NEUTRAL);
	servo_set_duty(TIMER_WINGTURN, WINGTURN_CENTER);
	end_switch_init();
	wings.angle = WINGTURN_RANGE / 2;
	wings.scan_direction = -1;
	wings_closing();
}

void wings_open(bool open)
{
	if (open)
		wings.target = STATE_OPEN;
	else
		wings.target = STATE_CLOSED;
}

void wings_scan(bool on)
{
	if (on) {
		wings.scan_direction = wings.last_scan_direction;
		if (!wings.scan_direction)
			wings.scan_direction = 1;
	} else {
		wings.last_scan_direction = wings.scan_direction;
		wings.scan_direction = 0;
	}
}

bool wings_opened(void)
{
	return wings.state == STATE_OPEN;
}

bool wings_closed(void)
{
	return !gpio_get_level(GPIO_END_SWITCH);
}

void wings_tick(void)
{
	switch (wings.state) {
	case STATE_OPENING:
		++wings.tick;
		servo_set_duty(TIMER_WINGSPAN,
			       interpolate(wings.tick, WINGSPAN_RAMP_TIME,
					   WINGSPAN_OPEN_START, WINGSPAN_OPEN_END));
		if (wings.target == STATE_CLOSED) {
			wings_closing();
		} else if (wings.tick >= TICKS_OPENING_DEAD && wings_closed()) {
			wings_broken();
		} else if (wings.tick >= TICKS_OPENING) {
			wings.state = STATE_OPEN;
			servo_set_duty(TIMER_WINGSPAN, WINGSPAN_NEUTRAL);
		}
		break;

	case STATE_OPEN:
		if (wings.target == STATE_CLOSED) {
			wings_closing();
		} else if (wings.scan_direction) {
			wings.angle += wings.scan_direction;
			if (wings.angle < 0) {
				wings.angle = 0;
				wings.scan_direction = 1;
			} else if (wings.angle > WINGTURN_RANGE) {
				wings.angle = WINGTURN_RANGE;
				wings.scan_direction = -1;
			}
			wings_set_turn();
		}
		break;

	case STATE_CENTERING:
		if (wings.target == STATE_OPEN) {
			wings.state = wings.target;
		} else if (wings.angle == WINGTURN_RANGE / 2) {
			wings.state = STATE_CLOSING;
			servo_set_duty(TIMER_WINGSPAN, WINGSPAN_CLOSE_START);
			wings.tick = 0;
		} else {
			if (wings.angle > WINGTURN_RANGE / 2)
				--wings.angle;
			else
				++wings.angle;
			wings_set_turn();
		}
		break;

	case STATE_CLOSING:
		++wings.tick;
		servo_set_duty(TIMER_WINGSPAN,
			       interpolate(wings.tick, WINGSPAN_RAMP_TIME,
					   WINGSPAN_CLOSE_START, WINGSPAN_CLOSE_END));
		if (wings_closed()) {
			wings.state = STATE_CLOSED;
			servo_set_duty(TIMER_WINGSPAN, WINGSPAN_NEUTRAL);
		} else if (++wings.tick > TICKS_CLOSE_TIMEOUT) {
			wings_broken();
		}
		break;

	case STATE_CLOSED:
		if (wings.target == STATE_OPEN) {
			wings.state = STATE_OPENING;
			servo_set_duty(TIMER_WINGSPAN, WINGSPAN_OPEN_START);
			wings.tick = 0;
		}
		break;
	}
}
