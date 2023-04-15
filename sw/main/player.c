#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2s.h"

#include "player.h"

/*---------------------------------------------------------------
  EXAMPLE CONFIG
  ---------------------------------------------------------------*/
#define PLAYER_PERIOD_SIZE	(1024)
#define PLAYER_I2S_PERIOD_SIZE	(2 * PLAYER_PERIOD_SIZE)
//i2s number
#define PLAYER_I2S_NUM		(0)
//i2s sample rate
#define PLAYER_I2S_SAMPLE_RATE	(22050)
//i2s data bits
#define PLAYER_I2S_SAMPLE_BITS	(16)
//I2S data format
#define PLAYER_I2S_FORMAT	(I2S_CHANNEL_FMT_RIGHT_LEFT)
//I2S channel number
#define PLAYER_I2S_CHANNEL_NUM	((PLAYER_I2S_FORMAT < I2S_CHANNEL_FMT_ONLY_RIGHT) ? (2) : (1))

#define PLAYER_LOGIC_MIN	(-128)
#define PLAYER_LOGIC_MAX	(127)

#define PLAYER_MASTER_RANGE	256
#define PLAYER_MASTER_OFFSET	(40)
#define PLAYER_MASTER_VOLUME	(PLAYER_MASTER_RANGE - PLAYER_MASTER_OFFSET)

/**
 * @brief I2S DAC mode init.
 */
static void player_i2s_init(void)
{
	int i2s_num = PLAYER_I2S_NUM;
	i2s_config_t i2s_config = {
		.mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
		.sample_rate =  PLAYER_I2S_SAMPLE_RATE,
		.bits_per_sample = PLAYER_I2S_SAMPLE_BITS,
		.communication_format = I2S_COMM_FORMAT_STAND_MSB,
		.channel_format = PLAYER_I2S_FORMAT,
		.intr_alloc_flags = 0,
		.dma_buf_count = 2,
		.dma_buf_len = 256,
		.use_apll = 1,
	};
	//install and start i2s driver
	i2s_driver_install(i2s_num, &i2s_config, 0, NULL);
	//init DAC pad
	i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
}

static int player_i2s_dac_sample_scale(uint8_t *buf, int sample)
{
	sample = (PLAYER_MASTER_VOLUME * sample) / PLAYER_MASTER_RANGE +
		PLAYER_MASTER_OFFSET;
#if (PLAYER_I2S_SAMPLE_BITS == 16)
	buf[0] = 0;
	buf[1] = sample;
	return 2;
#else
	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = sample;
	return 4;
#endif
}

struct player_stream_struct
{
	struct player_stream_struct *next;
	int offset;
	int size;
	FILE *file;
	int8_t buf[PLAYER_PERIOD_SIZE];
};

struct player_struct
{
	enum {
		STATE_SILENT,
		STATE_PLAYING,
	} state;
	struct player_stream_struct *stream;
	SemaphoreHandle_t lock;
};

static struct player_struct player;

static const uint8_t i2s_ramp_up_buff[] = {
#include "player-rampup.inc"
};

static const uint8_t i2s_ramp_down_buff[] = {
#include "player-rampdown.inc"
};

static void player_lock(struct player_struct *player)
{
	xSemaphoreTake(player->lock, portMAX_DELAY);
}

static void player_unlock(struct player_struct *player)
{
	xSemaphoreGive(player->lock);
}

static bool player_fill_streams(struct player_stream_struct *stream)
{
	bool active = false;

	while (stream) {
		if (fread(stream->buf, 1, sizeof(stream->buf), stream->file))
			active = true;
		stream = stream->next;
	}
	return active;
}

static bool player_active_streams(const struct player_stream_struct *stream)
{
	while (stream) {
		if (stream->offset < stream->size)
			return true;
		stream = stream->next;
	}
	return false;
}

static int player_mix(uint8_t *buf, struct player_stream_struct *stm)
{
	struct player_stream_struct *stream;
	int off = 0;
	int i;

	for (i = 0; i < PLAYER_PERIOD_SIZE; ++i) {
		int v = 0;

		for (stream = stm; stream; stream = stream->next) {
			int offset = stream->offset + i;

			if (offset < stream->size)
				v += stream->buf[i];
		}
		if (v > PLAYER_LOGIC_MAX)
			v = PLAYER_LOGIC_MAX;
		else if (v < PLAYER_LOGIC_MIN)
			v = PLAYER_LOGIC_MIN;

		off += player_i2s_dac_sample_scale(buf + off,
						   v - PLAYER_LOGIC_MIN);
	}

	for (stream = stm; stream; stream = stream->next) {
		int offset = stream->offset + PLAYER_PERIOD_SIZE;

		if (offset < stream->size)
			stream->offset = offset;
		else
			stream->offset = stream->size;
	}
	return off;
}

static void player_task(void *arg)
{
	struct player_struct *player = arg;
	uint8_t* i2s_write_buff = malloc(PLAYER_I2S_PERIOD_SIZE);

	i2s_set_clk(PLAYER_I2S_NUM, PLAYER_I2S_SAMPLE_RATE,
		    PLAYER_I2S_SAMPLE_BITS, 1);

	for (;;) {
		size_t bytes_written;
		int i2s_write_len;
		bool active;

		player_lock(player);

		switch (player->state) {
		case STATE_SILENT:
			active = player_active_streams(player->stream);
			player_unlock(player);
			if (active) {
				i2s_write(PLAYER_I2S_NUM,
					  i2s_ramp_up_buff, sizeof(i2s_ramp_up_buff),
					  &bytes_written, portMAX_DELAY);
				player->state = STATE_PLAYING;
			} else {
				vTaskDelay(10 / portTICK_PERIOD_MS);
			}
			break;

		case STATE_PLAYING:
			if (player_fill_streams(player->stream)) {
				i2s_write_len = player_mix(i2s_write_buff, player->stream);
				player_unlock(player);
				i2s_write(PLAYER_I2S_NUM, i2s_write_buff, i2s_write_len,
					  &bytes_written, portMAX_DELAY);
				player_lock(player);
			}
			active = player_active_streams(player->stream);
			player_unlock(player);
			if (!active) {
				i2s_write(PLAYER_I2S_NUM,
					  i2s_ramp_down_buff, sizeof(i2s_ramp_down_buff),
					  &bytes_written, portMAX_DELAY);

				memset(i2s_write_buff, 0, PLAYER_PERIOD_SIZE * PLAYER_I2S_SAMPLE_BITS / 8);
				i2s_write(PLAYER_I2S_NUM,
					  i2s_write_buff, PLAYER_PERIOD_SIZE * PLAYER_I2S_SAMPLE_BITS / 8,
					  &bytes_written, portMAX_DELAY);
				player->state = STATE_SILENT;
			}
			break;
		}
	}
	free(i2s_write_buff);
	vTaskDelete(NULL);
}

void player_init(void)
{
	player_i2s_init();
	player.lock = xSemaphoreCreateMutex();
	xTaskCreate(player_task, "player_task", 1024 * 2, &player, 5, NULL);
}

void *player_play(const char *name)
{
	FILE *file;
	struct player_stream_struct *stream;

	file = fopen(name, "r");
	if (!file)
		return NULL;

	stream = malloc(sizeof(*stream));
	if (!stream) {
		fclose(file);
		return NULL;
	}

	stream->offset = 0;
	fseek(file, 0, SEEK_END);
	stream->size = ftell(file);
	fseek(file, 0, SEEK_SET);
	stream->file = file;
	player_lock(&player);
	stream->next = player.stream;
	player.stream = stream;
	player_unlock(&player);

	return stream;
}

void player_close_stream(void *p)
{
	struct player_stream_struct *stream = p;
	struct player_stream_struct **c;

	player_lock(&player);
	for (c = &player.stream; *c; c = &(*c)->next) {
		if (*c == stream) {
			*c = (*c)->next;
			break;
		}
	}
	player_unlock(&player);
	fclose(stream->file);
	free(stream);
}

bool player_is_playing(void *p)
{
	struct player_stream_struct *stream = p;
	return stream->offset < stream->size;
}
