#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "accel.h"
#include "guns.h"
#include "player.h"
#include "wings.h"

#define GPIO_PIR	22
#define GPIO_LASER	14

#define RANDOM_CHANCE(p)	(random() < (long)((p) * 0x7fffffff))

static bool mount_fatfs(const char* partition_label)
{
	ESP_LOGI(__func__, "Mounting FAT filesystem");
	const esp_vfs_fat_mount_config_t mount_config = {
		.max_files = 4,
		.format_if_mount_failed = true,
		.allocation_unit_size = 512,
	};
	esp_err_t err = esp_vfs_fat_rawflash_mount("/audio", partition_label,
						   &mount_config);
	if (err != ESP_OK) {
		ESP_LOGE(__func__, "Failed to mount FATFS (%s)",
			 esp_err_to_name(err));
		return false;
	}
	return true;
}

static void pir_init(void)
{
	gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_INPUT,
		.pin_bit_mask = (1 << GPIO_PIR),
	};
	gpio_config(&io_conf);
}

static bool pir_target_detected(void)
{
	return gpio_get_level(GPIO_PIR);
}

static void laser_init(void)
{
	gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = (1 << GPIO_LASER),
	};
	gpio_config(&io_conf);
}

static void laser_on(bool on)
{
	gpio_set_level(GPIO_LASER, on);
}

enum {
	STATE_STABLE,
	STATE_WOBBLY,
	STATE_UNSTABLE,
	STATE_FALLEN,
};

enum {
	STATE_SEARCH,
	STATE_OPENING,
	STATE_FIRING,
	STATE_LOSING,
	STATE_LOST,
	STATE_ABOUT_TO_CLOSE,
	STATE_CLOSING,
};

enum {
	TRANSITION_NONE,
	TRANSITION_FIRING,
};

struct stable_struct {
	int state;
	void *stream;
	int ticks;
};

struct turret_struct {
	int state;
	void *stream;
	int ticks;
	struct stable_struct stable;
};

static struct turret_struct turret;

static void turret_close_stream(void **stream)
{
	if (*stream) {
		player_close_stream(*stream);
		*stream = NULL;
	}
}

static void turret_play_one_of(void **stream, const char * const name[])
{
	int i;

	for (i = 0; name[i]; ++i);
	turret_close_stream(stream);
	*stream = player_play(name[random() % i]);
}

static void stable_tick(struct stable_struct *stable)
{
	bool target_detected = pir_target_detected();
	int transition = TRANSITION_NONE;

	if (stable->stream && !player_is_playing(stable->stream))
		turret_close_stream(&stable->stream);

	switch (stable->state) {
	case STATE_SEARCH:
		if (target_detected) {
			wings_open(true);
			stable->state = STATE_OPENING;
			ESP_LOGI(__func__, "opening\n");
			stable->ticks = 0;
			stable->stream = player_play("/audio/09/013_alert.mp3.s8");
		}
		break;

	case STATE_OPENING:
		if (!stable->stream && wings_opened()) {
			stable->state = STATE_FIRING;
			ESP_LOGI(__func__, "firing\n");
			guns_fire(true);
		}
		break;

	case STATE_FIRING:
		if (!target_detected) {
			stable->state = STATE_LOSING;
			ESP_LOGI(__func__, "losing\n");
			guns_fire(false);
			stable->ticks = 0;
		}
		break;

	case STATE_LOSING:
		if (target_detected) {
			transition = TRANSITION_FIRING;
		} else if (stable->ticks > 100) {
			wings_scan(true);
			stable->ticks = 0;
			if (RANDOM_CHANCE(0.7)) {
				stable->state = STATE_LOST;
				ESP_LOGI(__func__, "lost\n");
				/* are you still there? */
				turret_play_one_of(&stable->stream,
						   (const char * const []){
						   "/audio/07/002_search.mp3.s8",
						   "/audio/07/005_search.mp3.s8",
						   "/audio/07/006_autosearch.mp3.s8",
						   "/audio/07/010_autosearch.mp3.s8",
						   NULL,
						   });
			}
		}
		break;

	case STATE_LOST:
		if (target_detected) {
			transition = TRANSITION_FIRING;
		} else if (!stable->stream) {
			if (stable->ticks > 100) {
				stable->ticks = 0;
				if (RANDOM_CHANCE(0.2)) {
					stable->state = STATE_ABOUT_TO_CLOSE;
					ESP_LOGI(__func__, "about to close\n");
				}
				if (RANDOM_CHANCE(0.1)) {
					stable->state = STATE_LOSING;
					ESP_LOGI(__func__, "losing\n");
				}
			}
		}
		break;

	case STATE_ABOUT_TO_CLOSE:
		if (target_detected) {
			transition = TRANSITION_FIRING;
		} else if (stable->ticks > 100) {
			stable->state = STATE_CLOSING;
			ESP_LOGI(__func__, "closing\n");
			wings_open(false);
			/* hibernating */
			turret_play_one_of(&stable->stream,
					   (const char * const []){
					   "/audio/07/003_search.mp3.s8",
					   "/audio/06/001_retire.mp3.s8",
					   "/audio/06/002_retire.mp3.s8",
					   "/audio/06/003_retire.mp3.s8",
					   NULL,
					   });
		}
		break;

	case STATE_CLOSING:
		if (!stable->stream && wings_closed()) {
			ESP_LOGI(__func__, "search\n");
			stable->state = STATE_SEARCH;
		}
		break;
	}
	++stable->ticks;

	switch (transition) {
	case TRANSITION_FIRING:
		if (!stable->stream) {
			/* there you are */
			turret_play_one_of(&stable->stream,
					   (const char * const []){
					   "/audio/01/002_active.mp3.s8",
					   "/audio/01/007_active.mp3.s8",
					   "/audio/01/008_active.mp3.s8",
					   NULL,
					   });
		}
		stable->state = STATE_FIRING;
		ESP_LOGI(__func__, "firing\n");
		wings_scan(false);
		guns_fire(true);
		break;
	}
}

static void turret_tick(struct turret_struct *turret)
{
	if (turret->stream && !player_is_playing(turret->stream))
		turret_close_stream(&turret->stream);

	switch (turret->state) {
	case STATE_STABLE:
		laser_on(true);
		if (accel_unstable()) {
			turret->state = STATE_WOBBLY;
			ESP_LOGI(__func__, "wobbly\n");
			guns_fire(false);
			wings_scan(false);
			turret->ticks = 0;
		} else {
			stable_tick(&turret->stable);
		}
		break;

	case STATE_WOBBLY:
		laser_on(turret->ticks & 0x10);
		if (turret->ticks > 10) {
			turret->state = STATE_UNSTABLE;
			ESP_LOGI(__func__, "unstable\n");
			turret_close_stream(&turret->stable.stream);
			/* put me down */
			turret_play_one_of(&turret->stream,
					   (const char * const []){
					   "/audio/05/001_pickup.mp3.s8",
					   "/audio/05/005_pickup.mp3.s8",
					   "/audio/05/006_pickup.mp3.s8",
					   "/audio/05/007_pickup.mp3.s8",
					   "/audio/05/008_pickup.mp3.s8",
					   NULL,
					   });
		}
		break;

	case STATE_UNSTABLE:
		laser_on(turret->ticks & 0x10);
		if (turret->ticks > 100) {
			if (accel_uneven()) {
				turret->state = STATE_FALLEN;
				ESP_LOGI(__func__, "fallen\n");
				wings_open(false);
				laser_on(false);
				turret->stable.state = STATE_SEARCH;
				/* critical error */
				turret_play_one_of(&turret->stream,
						   (const char * const []){
						   "/audio/08/003_tipped.mp3.s8",
						   "/audio/08/001_tipped.mp3.s8",
						   "/audio/04/003_disabled.mp3.s8",
						   "/audio/04/008_disabled.mp3.s8",
						   NULL,
						   });
				turret->ticks = 0;
			} else {
				turret->state = STATE_STABLE;
				ESP_LOGI(__func__, "stable\n");
			}
		} else if (accel_unstable()) {
			turret->ticks %= 0x20;
			if (!turret->stream && RANDOM_CHANCE(0.3)) {
				/* put me down */
				turret_play_one_of(&turret->stream,
						   (const char * const []){
						   "/audio/05/001_pickup.mp3.s8",
						   "/audio/05/005_pickup.mp3.s8",
						   "/audio/05/006_pickup.mp3.s8",
						   "/audio/05/007_pickup.mp3.s8",
						   "/audio/05/008_pickup.mp3.s8",
						   NULL,
						   });
			}
		}
		break;

	case STATE_FALLEN:
		if (accel_uneven()) {
			turret->ticks = 0;
		} else if (turret->ticks > 1000) {
			turret->state = STATE_STABLE;
			ESP_LOGI(__func__, "stable\n");
		}
		break;
	}
	++turret->ticks;
}

esp_err_t app_main(void)
{
	srand(esp_random());
	mount_fatfs("storage");
	laser_init();
	player_init();
	accel_init();
	pir_init();
	guns_init();
	wings_init();

	for (;;) {
		turret_tick(&turret);
		accel_tick();
		guns_tick();
		wings_tick();
		vTaskDelay(10 / portTICK_PERIOD_MS);
	}
	return ESP_OK;
}
