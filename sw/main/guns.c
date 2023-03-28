#include "driver/gpio.h"

#include "guns.h"
#include "player.h"

#define GPIO_LGUNS	13
#define GPIO_RGUNS	14

struct gun_struct {
	enum {
	       STATE_GUN_OFF,
	       STATE_GUN_ON,
	       STATE_GUN_BURST_PAUSE,
	} state;
	int tick;
	int burst_count;
	int gpio;
};

struct guns_struct {
	enum {
		STATE_OFF,
		STATE_FIRE,
	} state;
	void *stream;
	struct gun_struct gun[2];
};

static struct guns_struct guns;

static void gun_reset(struct gun_struct *gun)
{
	gun->state = STATE_GUN_OFF;
	gun->tick = 0;
	gun->burst_count = 0;
	gpio_set_level(gun->gpio, 0);
}

static void gun_tick(struct gun_struct *gun)
{
	switch (gun->state) {
	case STATE_GUN_ON:
		if (++gun->tick >= 4) {
			if (++gun->burst_count >= 4)
				gun->state = STATE_GUN_BURST_PAUSE;
			else
				gun->state = STATE_GUN_OFF;
			gun->tick = 0;
		}
		break;

	case STATE_GUN_OFF:
		if (++gun->tick >= 3) {
			gun->state = STATE_GUN_ON;
			gun->tick = 0;
		}
		break;

	case STATE_GUN_BURST_PAUSE:
		if (++gun->tick >= 22) {
			gun->state = STATE_GUN_ON;
			gun->burst_count = 0;
			gun->tick = 0;
		}
		break;
	}

	if (gun->tick == 0)
		gpio_set_level(gun->gpio, gun->state == STATE_GUN_ON);
}

void guns_init(void)
{
	gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = (1 << GPIO_LGUNS) | (1 << GPIO_RGUNS),
	};
	gpio_config(&io_conf);

	guns.gun[0].gpio = GPIO_LGUNS;
	guns.gun[1].gpio = GPIO_RGUNS;
}

void guns_fire(bool on)
{
	bool reset = false;

	if (on && guns.state != STATE_FIRE) {
		guns.state = STATE_FIRE;
		guns.stream = player_play("/audio/09/007_turret_firex3.mp3.s8");
		reset = true;
	} else if (!on && guns.state == STATE_FIRE) {
		guns.state = STATE_OFF;
		player_close_stream(guns.stream);
		reset = true;
	}
	if (reset) {
		gun_reset(guns.gun);
		gun_reset(guns.gun + 1);
	}
}

void guns_tick(void)
{
	if (guns.state == STATE_FIRE) {
		gun_tick(guns.gun);
		gun_tick(guns.gun + 1);
		if (!player_is_playing(guns.stream)) {
			player_close_stream(guns.stream);
			guns.stream = player_play("/audio/09/007_turret_firex3.mp3.s8");
		}
	}
}
