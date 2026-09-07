/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * XIAO nRF52840 energy-harvesting BLE sensor, Protocol v3.
 *
 * One image supports a bare XIAO or Logger HAT V3 and USB or external power.
 * The HAT rail is off between samples. Bare mode retains the original die
 * temperature/VDD behavior; HAT mode reads the raw SHT40, BH1750 and VSTOR
 * divider without instantiating boot-time sensor drivers.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_power.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/util.h>

/* Release builds set CONFIG_LOG=n, so all LOG_* calls disappear. */
LOG_MODULE_REGISTER(sensor, LOG_LEVEL_INF);

/* -------------------------------------------------------------------------
 * Timing and validation constants
 * ------------------------------------------------------------------------- */

#define SAMPLE_INTERVAL_MS CONFIG_SENSOR_SAMPLE_INTERVAL_MS
#define HAT_REDISCOVERY_INTERVAL_MS CONFIG_SENSOR_HAT_REDISCOVERY_INTERVAL_MS
#define VSTOR_MIN_MV CONFIG_SENSOR_VSTOR_MIN_MV
#define VSTOR_MAX_MV CONFIG_SENSOR_VSTOR_MAX_MV

/* 0x0140 * 0.625 ms = 200 ms; ~five events in the default one-second burst. */
#define ADV_INTERVAL_UNITS 0x0140
#define ADV_BURST_MS CONFIG_SENSOR_ADV_BURST_MS
#define BURST_DURATION K_MSEC(ADV_BURST_MS)

#define HAT_STARTUP_PROBE_ATTEMPTS 3U
#define HAT_DEMOTE_FAILURES 3U
#define HAT_PROBE_RETRY_DELAY_MS 5
#define HAT_POWERUP_DELAY_MS 1
#define VSTOR_SETTLE_MS 25
#define SHT40_CONVERSION_MS 10
#define BH1750_CONVERSION_MS 180
/* Includes the longest conversion plus rail, I2C, ADC and scheduler margin. */
#define MEASUREMENT_BUDGET_MS 250

#define SHT40_ADDR 0x44U
#define BH1750_ADDR 0x23U

#define SHT40_CMD_HIGH_PRECISION 0xFDU
#define SHT40_CMD_SOFT_RESET 0x94U
#define BH1750_CMD_POWER_ON 0x01U
#define BH1750_CMD_ONE_TIME_H_RES 0x20U

#define TEMP_INVALID INT16_MIN
#define HUMIDITY_INVALID UINT16_MAX
#define ILLUMINANCE_INVALID 0xFFFFFFU
#define VOLTAGE_INVALID UINT16_MAX

#define CONFIG_BARE_USB 1U
#define CONFIG_BARE_EXTERNAL 2U
#define CONFIG_HAT_USB 3U
#define CONFIG_HAT_EXTERNAL 4U

BUILD_ASSERT(SAMPLE_INTERVAL_MS > 0 &&
		     SAMPLE_INTERVAL_MS <= 0xFFFFFF,
	     "sample interval must fit BTHome uint24 milliseconds");
BUILD_ASSERT(HAT_REDISCOVERY_INTERVAL_MS > 0,
	     "HAT rediscovery interval must be positive");
/*
 * The burst is part of the cycle, not additional to it. Leaving room for the
 * measurement phase keeps the advertised cadence honest: if the burst ever
 * consumed the whole interval, the cycle would overrun and the sample rate
 * would silently drift away from the value transmitted in object 0x54.
 */
BUILD_ASSERT(ADV_BURST_MS > 0 &&
		     ADV_BURST_MS + MEASUREMENT_BUDGET_MS <= SAMPLE_INTERVAL_MS,
	     "sample interval must include the advertising burst and measurement budget");
BUILD_ASSERT(VSTOR_MIN_MV <= VSTOR_MAX_MV,
	     "VSTOR plausible range is inverted");
BUILD_ASSERT(NRF_POWER_HAS_USBREG == 1,
	     "target POWER peripheral must expose VBUS detection");

/* -------------------------------------------------------------------------
 * Protocol v3 BTHome service data
 * ------------------------------------------------------------------------- */

#define BARE_SVC_DATA_LEN 19U
#define HAT_SVC_DATA_LEN 26U
#define BARE_ADV_TOTAL_LEN 24U
#define HAT_ADV_TOTAL_LEN 31U

enum bare_offset {
	BARE_OFF_SEQ = 4,
	BARE_OFF_TEMP = 6,
	BARE_OFF_VOLTAGE = 9,
	BARE_OFF_INTERVAL = 13,
	BARE_OFF_CONFIG = 17,
};

enum hat_offset {
	HAT_OFF_SEQ = 4,
	HAT_OFF_TEMP = 6,
	HAT_OFF_HUMIDITY = 9,
	HAT_OFF_ILLUMINANCE = 12,
	HAT_OFF_VSTOR = 16,
	HAT_OFF_INTERVAL = 20,
	HAT_OFF_CONFIG = 24,
};

/*
 * Enhanced bare form:
 * D2 FC 40 00 ss 02 tt tt 0C vv vv 54 03 ii ii ii F0 cc cc
 */
static uint8_t bare_svc_data[BARE_SVC_DATA_LEN] = {
	0xD2, 0xFC, 0x40,
	0x00, 0x00,
	0x02, 0x00, 0x80,
	0x0C, 0xFF, 0xFF,
	0x54, 0x03, 0x00, 0x00, 0x00,
	0xF0, 0x00, 0x00,
};

/*
 * HAT form:
 * D2 FC 40 00 ss 02 tt tt 03 hh hh 05 ll ll ll 0C vv vv
 * 54 03 ii ii ii F0 cc cc
 */
static uint8_t hat_svc_data[HAT_SVC_DATA_LEN] = {
	0xD2, 0xFC, 0x40,
	0x00, 0x00,
	0x02, 0x00, 0x80,
	0x03, 0xFF, 0xFF,
	0x05, 0xFF, 0xFF, 0xFF,
	0x0C, 0xFF, 0xFF,
	0x54, 0x03, 0x00, 0x00, 0x00,
	0xF0, 0x00, 0x00,
};

BUILD_ASSERT(sizeof(bare_svc_data) == BARE_SVC_DATA_LEN,
	     "enhanced bare service data must be exactly 19 bytes");
BUILD_ASSERT(sizeof(hat_svc_data) == HAT_SVC_DATA_LEN,
	     "HAT service data must be exactly 26 bytes");
BUILD_ASSERT(3U + 2U + BARE_SVC_DATA_LEN == BARE_ADV_TOTAL_LEN,
	     "enhanced bare advertising data must total 24 bytes");
BUILD_ASSERT(3U + 2U + HAT_SVC_DATA_LEN == HAT_ADV_TOTAL_LEN,
	     "HAT advertising data must total the 31-byte legacy maximum");

static struct bt_data bare_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_SVC_DATA16, bare_svc_data, sizeof(bare_svc_data)),
};

static struct bt_data hat_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_SVC_DATA16, hat_svc_data, sizeof(hat_svc_data)),
};

static const struct bt_le_adv_param adv_param = {
	.id = BT_ID_DEFAULT,
	.sid = 0,
	.secondary_max_skip = 0,
	.options = BT_LE_ADV_OPT_USE_IDENTITY,
	.interval_min = ADV_INTERVAL_UNITS,
	.interval_max = ADV_INTERVAL_UNITS,
	.peer = NULL,
};

/* -------------------------------------------------------------------------
 * Build-time device mapping
 * ------------------------------------------------------------------------- */

#define USER_NODE DT_PATH(zephyr_user)

static const struct device *const temp_dev =
	DEVICE_DT_GET_ONE(nordic_nrf_temp);
static const struct device *const i2c_bus =
	DEVICE_DT_GET(DT_NODELABEL(i2c1));

static const struct adc_dt_spec vdd_adc =
	ADC_DT_SPEC_GET_BY_NAME(USER_NODE, vdd);
static const struct adc_dt_spec vstor_adc =
	ADC_DT_SPEC_GET_BY_NAME(USER_NODE, vstor);
static const struct gpio_dt_spec hat_enable =
	GPIO_DT_SPEC_GET(USER_NODE, hat_enable_gpios);

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_qspi_nor)
static const struct device *const qspi_dev =
	DEVICE_DT_GET_ONE(nordic_qspi_nor);
#define HAVE_QSPI 1
#else
#define HAVE_QSPI 0
#endif

#define LED_GPIO(alias) GPIO_DT_SPEC_GET_OR(DT_ALIAS(alias), gpios, {0})
static const struct gpio_dt_spec leds[] = {
	LED_GPIO(led0),
	LED_GPIO(led1),
	LED_GPIO(led2),
};

static bool temp_ready;
static bool vdd_adc_ready;
static bool vstor_adc_ready;
static bool hat_io_ready;

/* -------------------------------------------------------------------------
 * Measurement types and encoding helpers
 * ------------------------------------------------------------------------- */

struct sample {
	int16_t temp_centi;
	uint16_t voltage_mv;
	uint16_t humidity_centi;
	uint32_t illuminance_centi;
	uint8_t config_id;
	bool hat_present;
	bool usb_present;
};

struct hat_session {
	bool i2c_held;
	bool rail_enabled;
	int64_t rail_on_ms;
};

static void sample_reset(struct sample *sample)
{
	sample->temp_centi = TEMP_INVALID;
	sample->voltage_mv = VOLTAGE_INVALID;
	sample->humidity_centi = HUMIDITY_INVALID;
	sample->illuminance_centi = ILLUMINANCE_INVALID;
	sample->config_id = 0U;
	sample->hat_present = false;
	sample->usb_present = false;
}

static void put_le16(uint8_t *dst, uint16_t value)
{
	dst[0] = (uint8_t)(value & 0xFFU);
	dst[1] = (uint8_t)(value >> 8);
}

static void put_le24(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)(value & 0xFFU);
	dst[1] = (uint8_t)((value >> 8) & 0xFFU);
	dst[2] = (uint8_t)((value >> 16) & 0xFFU);
}

static uint8_t configuration_id(bool hat_present, bool usb_present)
{
	if (hat_present) {
		return usb_present ? CONFIG_HAT_USB : CONFIG_HAT_EXTERNAL;
	}

	return usb_present ? CONFIG_BARE_USB : CONFIG_BARE_EXTERNAL;
}

static bool usb_vbus_present(void)
{
	/*
	 * Direct register/HAL read only: the USB device stack stays disabled.
	 * VBUSDETECT reflects the physical USB supply, independent of USBD.
	 */
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
}

static void sleep_until_ms(int64_t deadline_ms)
{
	int64_t remaining_ms = deadline_ms - k_uptime_get();

	if (remaining_ms > 0) {
		k_sleep(K_MSEC(remaining_ms));
	}
}

/* -------------------------------------------------------------------------
 * Bare-board measurements
 * ------------------------------------------------------------------------- */

static bool read_die_temperature_centi(int16_t *temp_centi)
{
	struct sensor_value val = {0};
	int err;

	if (!temp_ready) {
		return false;
	}

	err = sensor_sample_fetch(temp_dev);
	if (err == 0) {
		err = sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &val);
	}
	if (err) {
		LOG_WRN("die temperature read failed (%d)", err);
		return false;
	}

	int64_t micro_c = (int64_t)val.val1 * 1000000LL + val.val2;
	int64_t rounded_centi =
		(micro_c >= 0) ? (micro_c + 5000LL) / 10000LL
			       : (micro_c - 5000LL) / 10000LL;

	if (rounded_centi <= INT16_MIN || rounded_centi > INT16_MAX) {
		LOG_WRN("die temperature out of protocol range");
		return false;
	}

	*temp_centi = (int16_t)rounded_centi;
	return true;
}

static bool read_adc_input_mv(const struct adc_dt_spec *spec,
			      bool channel_ready, uint16_t *millivolts)
{
	int16_t raw = 0;
	struct adc_sequence sequence = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int err;
	bool success = false;

	if (!channel_ready) {
		return false;
	}

	err = pm_device_action_run(spec->dev, PM_DEVICE_ACTION_RESUME);
	if (err && err != -EALREADY && err != -ENOTSUP) {
		LOG_WRN("SAADC resume failed (%d)", err);
		return false;
	}

	err = adc_sequence_init_dt(spec, &sequence);
	if (err == 0) {
		err = adc_read_dt(spec, &sequence);
	}

	/* The shared SAADC is suspended immediately after every conversion. */
	(void)pm_device_action_run(spec->dev, PM_DEVICE_ACTION_SUSPEND);

	if (err) {
		LOG_WRN("SAADC read failed (%d)", err);
		return false;
	}
	if (raw < 0) {
		LOG_WRN("SAADC returned negative code");
		return false;
	}

	int32_t converted_mv = raw;

	err = adc_raw_to_millivolts_dt(spec, &converted_mv);
	if (err == 0 && converted_mv >= 0 &&
	    converted_mv < (int32_t)VOLTAGE_INVALID) {
		*millivolts = (uint16_t)converted_mv;
		success = true;
	}

	if (!success) {
		LOG_WRN("SAADC conversion to millivolts failed (%d)", err);
	}

	return success;
}

static bool read_vdd_mv(uint16_t *vdd_mv)
{
	uint16_t divided_mv;

	if (!read_adc_input_mv(&vdd_adc, vdd_adc_ready, &divided_mv)) {
		return false;
	}

	/*
	 * nRF52840's NRF_SAADC_VDD input samples the VDD rail directly; only
	 * VDDHDIV5 has a fixed internal divider. adc_raw_to_millivolts_dt()
	 * already accounts for the 0.6 V reference and 1/6 channel gain, so
	 * the converted value is the rail voltage.
	 */
	uint32_t converted_mv = divided_mv;

	if (converted_mv >= VOLTAGE_INVALID) {
		LOG_WRN("VDD outside protocol range (%u mV)", converted_mv);
		return false;
	}

	*vdd_mv = (uint16_t)converted_mv;
	return true;
}

static void measure_bare(struct sample *sample)
{
	(void)read_die_temperature_centi(&sample->temp_centi);
	(void)read_vdd_mv(&sample->voltage_mv);
}

/* -------------------------------------------------------------------------
 * Logger HAT V3 raw I2C and power-gating helpers
 * ------------------------------------------------------------------------- */

static int i2c_write_command(uint16_t address, uint8_t command)
{
	return i2c_write(i2c_bus, &command, sizeof(command), address);
}

static uint8_t sht40_crc8(const uint8_t *data, size_t length);

static void hat_session_end(struct hat_session *session)
{
	/*
	 * Release the controller first so its P0.04/P0.05 sleep pinctrl state is
	 * applied, then remove D10 power. SDA/SCL are never driven low while the
	 * unpowered sensors sit behind the HAT's always-on 10 kOhm pull-ups.
	 */
	if (session->i2c_held) {
		int err = pm_device_runtime_put(i2c_bus);

		if (err && err != -EALREADY) {
			LOG_WRN("i2c1 suspend failed (%d)", err);
		}
		session->i2c_held = false;
	}

	if (hat_io_ready) {
		int err = gpio_pin_set_dt(&hat_enable, 0);

		if (err) {
			LOG_WRN("D10 disable failed (%d)", err);
		}
	}
	session->rail_enabled = false;
}

static int hat_session_begin(struct hat_session *session)
{
	int err;

	session->i2c_held = false;
	session->rail_enabled = false;
	session->rail_on_ms = 0;

	if (!hat_io_ready) {
		return -ENODEV;
	}

	err = pm_device_runtime_get(i2c_bus);
	if (err) {
		LOG_WRN("i2c1 resume failed (%d)", err);
		goto cleanup;
	}
	session->i2c_held = true;

	err = gpio_pin_set_dt(&hat_enable, 1);
	if (err) {
		LOG_WRN("D10 enable failed (%d)", err);
		goto cleanup;
	}
	session->rail_enabled = true;
	session->rail_on_ms = k_uptime_get();

	/* Datasheet-safe sensor rail startup delay; this is sleeping, not busy. */
	k_sleep(K_MSEC(HAT_POWERUP_DELAY_MS));
	return 0;

cleanup:
	hat_session_end(session);
	return err;
}

static bool probe_sht40_verified(void)
{
	uint8_t data[6];

	if (i2c_write_command(SHT40_ADDR, SHT40_CMD_HIGH_PRECISION)) {
		return false;
	}
	k_sleep(K_MSEC(SHT40_CONVERSION_MS));
	if (i2c_read(i2c_bus, data, sizeof(data), SHT40_ADDR)) {
		return false;
	}

	/* A CRC-covered payload cannot come from a floating bus. */
	return sht40_crc8(&data[0], 2) == data[2] &&
	       sht40_crc8(&data[3], 2) == data[5];
}

static bool probe_bh1750_verified(void)
{
	uint8_t data[2];

	if (i2c_write_command(BH1750_ADDR, BH1750_CMD_POWER_ON) ||
	    i2c_write_command(BH1750_ADDR, BH1750_CMD_ONE_TIME_H_RES)) {
		return false;
	}
	k_sleep(K_MSEC(BH1750_CONVERSION_MS));
	if (i2c_read(i2c_bus, data, sizeof(data), BH1750_ADDR)) {
		return false;
	}

	/* An idle/floating bus reads back 0xFFFF; a real conversion does not. */
	return !(data[0] == 0xFFU && data[1] == 0xFFU);
}

static bool probe_hat_once(void)
{
	struct hat_session session;
	bool present = false;

	if (hat_session_begin(&session)) {
		goto cleanup;
	}

	/*
	 * One verified sensor is enough to prove the HAT is fitted. Requiring
	 * both would misclassify a HAT with one failed sensor as bare. Try the
	 * BH1750 only if the SHT40 verification fails to keep rail-on time short.
	 */
	present = probe_sht40_verified() || probe_bh1750_verified();

	if (!present) {
		LOG_INF("HAT probe miss: no verified sensor transaction");
	}

cleanup:
	hat_session_end(&session);
	return present;
}

static bool detect_hat(uint8_t attempts)
{
	for (uint8_t attempt = 0; attempt < attempts; attempt++) {
		if (probe_hat_once()) {
			LOG_INF("Logger HAT V3 detected");
			return true;
		}

		if (attempt + 1U < attempts) {
			k_sleep(K_MSEC(HAT_PROBE_RETRY_DELAY_MS));
		}
	}

	return false;
}

static uint8_t sht40_crc8(const uint8_t *data, size_t length)
{
	uint8_t crc = 0xFFU;

	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (uint8_t bit = 0; bit < 8U; bit++) {
			crc = (crc & 0x80U) ?
				(uint8_t)((crc << 1) ^ 0x31U) :
				(uint8_t)(crc << 1);
		}
	}

	return crc;
}

static bool decode_sht40(const uint8_t data[6], int16_t *temp_centi,
			 uint16_t *humidity_centi)
{
	if (sht40_crc8(&data[0], 2) != data[2] ||
	    sht40_crc8(&data[3], 2) != data[5]) {
		LOG_WRN("SHT40 CRC failure");
		return false;
	}

	uint16_t raw_temp = ((uint16_t)data[0] << 8) | data[1];
	uint16_t raw_humidity = ((uint16_t)data[3] << 8) | data[4];

	/* Rounded integer forms of the SHT4x datasheet conversions. */
	int32_t converted_temp =
		-4500 + (int32_t)(((uint32_t)17500U * raw_temp + 32767U) /
				  65535U);
	int32_t converted_humidity =
		-600 + (int32_t)(((uint32_t)12500U * raw_humidity + 32767U) /
				 65535U);

	converted_humidity = CLAMP(converted_humidity, 0, 10000);
	if (converted_temp <= INT16_MIN || converted_temp > INT16_MAX) {
		return false;
	}

	*temp_centi = (int16_t)converted_temp;
	*humidity_centi = (uint16_t)converted_humidity;
	return true;
}

static bool read_vstor_mv(uint16_t *vstor_mv)
{
	uint16_t vadc_mv;

	if (!read_adc_input_mv(&vstor_adc, vstor_adc_ready, &vadc_mv)) {
		return false;
	}

	/*
	 * Logger HAT V3 R4=100k/R5=100k, so VSTOR/VADC=2.0.
	 * TODO(hardware verification): confirm BAT+ is physically rewired to
	 * BQ25570 VSTOR and validate this divider ratio against a calibrated DMM.
	 */
	uint32_t converted_mv = (uint32_t)vadc_mv * 2U;

	if (converted_mv < VSTOR_MIN_MV || converted_mv > VSTOR_MAX_MV ||
	    converted_mv >= VOLTAGE_INVALID) {
		LOG_WRN("VSTOR outside plausible range (%u mV)", converted_mv);
		return false;
	}

	*vstor_mv = (uint16_t)converted_mv;
	return true;
}

static void measure_hat(struct sample *sample)
{
	struct hat_session session;
	bool bh1750_started = false;
	bool sht40_started = false;
	int64_t bh1750_deadline_ms = 0;
	int64_t sht40_deadline_ms = 0;
	uint8_t sht40_data[6];
	uint8_t bh1750_data[2];
	int err;

	if (hat_session_begin(&session)) {
		goto cleanup;
	}

	/* Start the longest conversion first to minimize shared rail-on time. */
	err = i2c_write_command(BH1750_ADDR, BH1750_CMD_POWER_ON);
	if (err == 0) {
		err = i2c_write_command(BH1750_ADDR,
					BH1750_CMD_ONE_TIME_H_RES);
	}
	if (err == 0) {
		bh1750_started = true;
		bh1750_deadline_ms = k_uptime_get() + BH1750_CONVERSION_MS;
	} else {
		LOG_WRN("BH1750 start failed (%d)", err);
	}

	err = i2c_write_command(SHT40_ADDR, SHT40_CMD_HIGH_PRECISION);
	if (err == 0) {
		sht40_started = true;
		sht40_deadline_ms = k_uptime_get() + SHT40_CONVERSION_MS;
	} else {
		LOG_WRN("SHT40 start failed (%d)", err);
	}

	if (sht40_started) {
		sleep_until_ms(sht40_deadline_ms);
		err = i2c_read(i2c_bus, sht40_data, sizeof(sht40_data),
			       SHT40_ADDR);
		if (err == 0) {
			(void)decode_sht40(sht40_data, &sample->temp_centi,
					   &sample->humidity_centi);
		} else {
			LOG_WRN("SHT40 read failed (%d)", err);
		}
	}

	/*
	 * The divider RC is approximately 5 ms. Wait five time constants before
	 * conversion, then suspend SAADC immediately in read_adc_input_mv().
	 * USB bench mode deliberately keeps the Protocol v3 voltage sentinel.
	 */
	if (!sample->usb_present) {
		sleep_until_ms(session.rail_on_ms + VSTOR_SETTLE_MS);
		(void)read_vstor_mv(&sample->voltage_mv);
	}

	if (bh1750_started) {
		sleep_until_ms(bh1750_deadline_ms);
		err = i2c_read(i2c_bus, bh1750_data, sizeof(bh1750_data),
			       BH1750_ADDR);
		if (err == 0) {
			uint16_t raw = ((uint16_t)bh1750_data[0] << 8) |
				       bh1750_data[1];

			/* lux = raw/1.2; BTHome object 0x05 is 0.01 lux. */
			uint32_t centi_lux =
				((uint32_t)raw * 250U + 1U) / 3U;

			sample->illuminance_centi =
				MIN(centi_lux, ILLUMINANCE_INVALID - 1U);
		} else {
			LOG_WRN("BH1750 read failed (%d)", err);
		}
	}

cleanup:
	/*
	 * One cleanup path handles success, NACK, CRC and ADC failures. Once a
	 * HAT has been detected these failures invalidate only this sample's
	 * affected fields; they do not change the persistent HAT capability.
	 */
	hat_session_end(&session);
}

static bool hat_sample_all_invalid(const struct sample *sample)
{
	return sample->temp_centi == TEMP_INVALID &&
	       sample->humidity_centi == HUMIDITY_INVALID &&
	       sample->illuminance_centi == ILLUMINANCE_INVALID;
}

/* -------------------------------------------------------------------------
 * Protocol encoding and advertising
 * ------------------------------------------------------------------------- */

static void encode_bare_payload(uint8_t sequence, const struct sample *sample)
{
	bare_svc_data[BARE_OFF_SEQ] = sequence;
	put_le16(&bare_svc_data[BARE_OFF_TEMP],
		 (uint16_t)sample->temp_centi);
	put_le16(&bare_svc_data[BARE_OFF_VOLTAGE], sample->voltage_mv);
	put_le24(&bare_svc_data[BARE_OFF_INTERVAL], SAMPLE_INTERVAL_MS);
	put_le16(&bare_svc_data[BARE_OFF_CONFIG], sample->config_id);
}

static void encode_hat_payload(uint8_t sequence, const struct sample *sample)
{
	hat_svc_data[HAT_OFF_SEQ] = sequence;
	put_le16(&hat_svc_data[HAT_OFF_TEMP],
		 (uint16_t)sample->temp_centi);
	put_le16(&hat_svc_data[HAT_OFF_HUMIDITY],
		 sample->humidity_centi);
	put_le24(&hat_svc_data[HAT_OFF_ILLUMINANCE],
		 sample->illuminance_centi);
	put_le16(&hat_svc_data[HAT_OFF_VSTOR], sample->voltage_mv);
	put_le24(&hat_svc_data[HAT_OFF_INTERVAL], SAMPLE_INTERVAL_MS);
	put_le16(&hat_svc_data[HAT_OFF_CONFIG], sample->config_id);
}

static int advertise_burst(bool hat_present)
{
	const struct bt_data *data = hat_present ? hat_ad : bare_ad;
	size_t data_count = hat_present ? ARRAY_SIZE(hat_ad) :
					 ARRAY_SIZE(bare_ad);
	int err;

	err = bt_le_adv_start(&adv_param, data, data_count, NULL, 0);
	if (err == -EALREADY) {
		static uint32_t already_active_count;
		already_active_count++;
		LOG_DBG("advertising unexpectedly active (%u); stop/retry",
			already_active_count);
		int stop_err = bt_le_adv_stop();

		if (stop_err) {
			LOG_WRN("advertising recovery stop failed (%d)", stop_err);
		}
		err = bt_le_adv_start(&adv_param, data, data_count, NULL, 0);
	}
	if (err) {
		LOG_ERR("advertising start failed (%d)", err);
		return err;
	}

	k_sleep(BURST_DURATION);

	err = bt_le_adv_stop();
	if (err) {
		LOG_ERR("advertising stop failed (%d)", err);
	}

	return err;
}

/* -------------------------------------------------------------------------
 * One-time low-power initialization
 * ------------------------------------------------------------------------- */

static void quiesce_leds(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (device_is_ready(leds[i].port)) {
			(void)gpio_pin_configure_dt(&leds[i],
						    GPIO_OUTPUT_INACTIVE);
		}
	}
}

static void quiesce_qspi_flash(void)
{
#if HAVE_QSPI
	if (!device_is_ready(qspi_dev)) {
		LOG_WRN("QSPI flash not ready; cannot enter DPD");
		return;
	}

	int err = pm_device_action_run(qspi_dev, PM_DEVICE_ACTION_SUSPEND);

	if (err && err != -EALREADY) {
		LOG_WRN("QSPI suspend failed (%d)", err);
	} else {
		LOG_INF("QSPI flash in deep power-down");
	}
#endif
}

static void initialize_adc(void)
{
	if (!adc_is_ready_dt(&vdd_adc) || !adc_is_ready_dt(&vstor_adc)) {
		LOG_ERR("SAADC device not ready");
		return;
	}

	(void)pm_device_action_run(vdd_adc.dev, PM_DEVICE_ACTION_RESUME);

	int vdd_err = adc_channel_setup_dt(&vdd_adc);
	int vstor_err = adc_channel_setup_dt(&vstor_adc);

	(void)pm_device_action_run(vdd_adc.dev, PM_DEVICE_ACTION_SUSPEND);

	vdd_adc_ready = (vdd_err == 0);
	vstor_adc_ready = (vstor_err == 0);
	if (vdd_err) {
		LOG_ERR("VDD ADC channel setup failed (%d)", vdd_err);
	}
	if (vstor_err) {
		LOG_ERR("VSTOR ADC channel setup failed (%d)", vstor_err);
	}
}

static void initialize_hat_io(void)
{
	if (!gpio_is_ready_dt(&hat_enable) || !device_is_ready(i2c_bus)) {
		LOG_ERR("Logger HAT GPIO/i2c1 not ready");
		return;
	}

	int err = gpio_pin_configure_dt(&hat_enable, GPIO_OUTPUT_INACTIVE);

	if (err) {
		LOG_ERR("D10 configuration failed (%d)", err);
		return;
	}

	/*
	 * i2c1 carries zephyr,pm-device-runtime-auto upstream. Explicitly enable
	 * runtime PM as a portability fallback; enabling suspends an active
	 * controller and applies its high-impedance sleep pinctrl state.
	 */
	if (!pm_device_runtime_is_enabled(i2c_bus)) {
		err = pm_device_runtime_enable(i2c_bus);
		if (err) {
			LOG_ERR("i2c1 runtime PM enable failed (%d)", err);
			return;
		}
	}

	hat_io_ready = true;
}

static void log_identity_addr(void)
{
#if defined(CONFIG_LOG) || defined(CONFIG_PRINTK)
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = CONFIG_BT_ID_MAX;
	char address[BT_ADDR_LE_STR_LEN];

	bt_id_get(addrs, &count);
	if (count > 0U) {
		bt_addr_le_to_str(&addrs[0], address, sizeof(address));
		LOG_INF("Identity (advertised) address: %s", address);
	}
#endif
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void)
{
	uint8_t sequence = 0U;
	uint8_t hat_failures = 0U;
	bool bt_ready = false;

	quiesce_leds();
	quiesce_qspi_flash();
	initialize_adc();
	initialize_hat_io();

	temp_ready = device_is_ready(temp_dev);
	if (!temp_ready) {
		LOG_ERR("die-temperature device not ready");
	}

	/* Small bounded startup retry before selecting the enhanced bare form. */
	bool hat_present = detect_hat(HAT_STARTUP_PROBE_ATTEMPTS);
	int64_t next_hat_probe_ms =
		k_uptime_get() + HAT_REDISCOVERY_INTERVAL_MS;

	while (true) {
		int64_t cycle_start_ms = k_uptime_get();
		struct sample sample;

		if (!bt_ready) {
			int err = bt_enable(NULL);

			if (err == 0 || err == -EALREADY) {
				bt_ready = true;
				log_identity_addr();
			} else {
				LOG_ERR("bt_enable failed (%d)", err);
			}
		}

		/*
		 * Retry discovery by elapsed time rather than cycle count, so a
		 * different configured sample interval requires no code change.
		 */
		if (!hat_present && cycle_start_ms >= next_hat_probe_ms) {
			hat_present = detect_hat(1U);
			next_hat_probe_ms =
				k_uptime_get() + HAT_REDISCOVERY_INTERVAL_MS;
		}

		sample_reset(&sample);
		sample.hat_present = hat_present;
		sample.usb_present = usb_vbus_present();
		sample.config_id =
			configuration_id(hat_present, sample.usb_present);

		if (hat_present) {
			measure_hat(&sample);

			if (hat_sample_all_invalid(&sample)) {
				hat_failures++;
			} else {
				hat_failures = 0U;
			}

			if (hat_failures >= HAT_DEMOTE_FAILURES) {
				LOG_WRN("HAT unresponsive; reverting to bare");
				hat_present = false;
				hat_failures = 0U;
				next_hat_probe_ms = k_uptime_get() +
					HAT_REDISCOVERY_INTERVAL_MS;
			}
		}

		if (hat_present) {
			encode_hat_payload(sequence, &sample);
		} else {
			if (sample.hat_present) {
				/*
				 * The HAT was demoted this cycle. Rebuild a bare
				 * sample so this record is valid as well.
				 */
				sample_reset(&sample);
				sample.usb_present = usb_vbus_present();
			}
			sample.hat_present = false;
			sample.config_id =
				configuration_id(false, sample.usb_present);
			measure_bare(&sample);
			encode_bare_payload(sequence, &sample);
		}

		LOG_INF("seq=%u config=%u temp=%d voltage=%u hum=%u lux_x100=%u",
			sequence, sample.config_id, sample.temp_centi,
			sample.voltage_mv, sample.humidity_centi,
			sample.illuminance_centi);

		sequence++;

		/* Broadcast even when individual measurements contain sentinels. */
		if (bt_ready) {
			(void)advertise_burst(hat_present);
		}

		int64_t elapsed_ms = k_uptime_get() - cycle_start_ms;
		int64_t remaining_ms =
			(int64_t)SAMPLE_INTERVAL_MS - elapsed_ms;

		if (remaining_ms < 0) {
			remaining_ms = 0;
		}
		k_sleep(K_MSEC(remaining_ms));
	}

	return 0;
}
