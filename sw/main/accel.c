#include "esp_log.h"
#include "driver/i2c.h"

#include "accel.h"

#define I2C_MASTER_SCL_IO		21
#define I2C_MASTER_SDA_IO		19
#define I2C_MASTER_NUM			0
#define I2C_MASTER_FREQ_HZ		400000
#define I2C_MASTER_TX_BUF_DISABLE	0
#define I2C_MASTER_RX_BUF_DISABLE	0
#define I2C_MASTER_TIMEOUT_MS		1000

#define ADXL345_ADDR			0x1d

#define ADXL345_ID_REG			0
#define ADXL345_ID				0xe5
#define ADXL345_POWER_CTL_REG		0x2d
#define ADXL345_POWER_CTL_MEASURE		0x8
#define ADXL345_DATA_FORMAT_REG		0x31
#define ADXL345_DATA_FORMAT_RANGE_2G		0x0
#define ADXL345_DATA_FORMAT_RANGE_4G		0x1
#define ADXL345_DATA_FORMAT_RANGE_8G		0x2
#define ADXL345_DATA_FORMAT_RANGE_16G		0x3
#define ADXL345_DATA_FORMAT_JUSTIFY		0x4
#define ADXL345_DATA_FORMAT_SELF_TEST		0x80
#define ADXL345_DATA_REG		0x32

#define N_LOG				128
#define ACCEL_G_Z			(-210)
#define ACCEL_G_2			(ACCEL_G_Z * ACCEL_G_Z)

struct p3d_struct {
	int x;
	int y;
	int z;
};

struct output_struct {
	int16_t x;
	int16_t y;
	int16_t z;
};

struct accel_struct
{
	int tick;
	struct p3d_struct average;
	struct output_struct log[N_LOG];
	int log_idx;
};

static struct accel_struct accel;

static esp_err_t adxl345_register_read(uint8_t reg_addr, void *data, size_t len)
{
	return i2c_master_write_read_device(I2C_MASTER_NUM, ADXL345_ADDR,
					    &reg_addr, 1, data, len,
					    I2C_MASTER_TIMEOUT_MS / portTICK_RATE_MS);
}

static esp_err_t adxl345_register_write_byte(uint8_t reg_addr, uint8_t data)
{
	int ret;
	uint8_t write_buf[2] = {reg_addr, data};

	ret = i2c_master_write_to_device(I2C_MASTER_NUM, ADXL345_ADDR,
					 write_buf, sizeof(write_buf),
					 I2C_MASTER_TIMEOUT_MS / portTICK_RATE_MS);

	return ret;
}

static esp_err_t i2c_master_init(void)
{
	i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = I2C_MASTER_SDA_IO,
		.scl_io_num = I2C_MASTER_SCL_IO,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = I2C_MASTER_FREQ_HZ,
	};

	ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
	return i2c_driver_install(I2C_MASTER_NUM, conf.mode,
				  I2C_MASTER_RX_BUF_DISABLE,
				  I2C_MASTER_TX_BUF_DISABLE, 0);
}

void accel_init(void)
{
	uint8_t id = 0;
	int i;

	ESP_ERROR_CHECK(i2c_master_init());
	for (i = 0; i < 5; ++i) {
		adxl345_register_read(ADXL345_ID_REG, &id, sizeof(id));
		if (id == ADXL345_ID)
			break;
		ESP_LOGD(__func__, "ID = 0x%02x\n", id);
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	adxl345_register_write_byte(ADXL345_DATA_FORMAT_REG, ADXL345_DATA_FORMAT_RANGE_2G);
	adxl345_register_write_byte(ADXL345_POWER_CTL_REG, ADXL345_POWER_CTL_MEASURE);

	for (i = 0; i < 10; ++i) {
		struct output_struct o;

		vTaskDelay(pdMS_TO_TICKS(10));
		adxl345_register_read(ADXL345_DATA_REG, &o, sizeof(o));
		ESP_LOGD(__func__, "x = %d, y = %d, z = %d\n", o.x, o.y, o.z);
	}
}

void accel_tick(void)
{
	struct output_struct o;

	if (accel.tick < N_LOG)
		++accel.tick;
	adxl345_register_read(ADXL345_DATA_REG, &o, sizeof(o));
	accel.average.x += o.x - accel.log[accel.log_idx].x;
	accel.average.y += o.y - accel.log[accel.log_idx].y;
	accel.average.z += o.z - accel.log[accel.log_idx].z;
	accel.log[accel.log_idx] = o;
	accel.log_idx = (accel.log_idx + 1) % N_LOG;
}

/* Average gravity vector length deviates from g by more than ~15% */
bool accel_unstable(void)
{
	int dx = accel.average.x / N_LOG;
	int dy = accel.average.y / N_LOG;
	int dz = accel.average.z / N_LOG;

	int diff = abs((dx * dx + dy * dy + dz * dz) - ACCEL_G_2);

	if (accel.tick < N_LOG)
		return false;

	//ESP_LOGI(__func__, "%d, %d, %d, diff = %d", dx, dy, dz, diff);
	return diff > ACCEL_G_2 / 32 || accel_uneven();
}

/* Gravity vector deviates from normal by more than 60 degrees */
bool accel_uneven(void)
{
	int dx = accel.average.x / N_LOG;
	int dy = accel.average.y / N_LOG;
	int dz = accel.average.z / N_LOG - ACCEL_G_Z;

	//ESP_LOGI(__func__, "%d, %d, %d", dx, dy, dz);
	return (dx * dx + dy * dy + dz * dz) > ACCEL_G_Z * ACCEL_G_Z;
}
