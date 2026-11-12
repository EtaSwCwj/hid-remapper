#include "battery.h"

#include <hal/nrf_saadc.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#define MEASURE_INTERVAL K_MINUTES(5)

static const struct device* const adc = DEVICE_DT_GET(DT_NODELABEL(adc));

// VDD measured directly: gain 1/6 with the 0.6 V internal reference gives a
// 0..3.6 V range.
static const struct adc_channel_cfg channel_cfg = {
    .gain = ADC_GAIN_1_6,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 10),
    .channel_id = 0,
    .input_positive = SAADC_CH_PSELP_PSELP_VDD,
};

static uint16_t millivolts;
static uint8_t percent = 100;

// CR2032 under a light load: flat around 2.9-3.0 V for most of its life,
// then it drops quickly.
static const struct {
    uint16_t mv;
    uint8_t pct;
} curve[] = {
    { 3000, 100 },
    { 2900, 80 },
    { 2800, 60 },
    { 2700, 40 },
    { 2600, 20 },
    { 2400, 5 },
    { 2000, 0 },
};

static uint8_t mv_to_percent(uint16_t mv) {
    if (mv >= curve[0].mv) {
        return 100;
    }
    for (int i = 1; i < ARRAY_SIZE(curve); i++) {
        if (mv >= curve[i].mv) {
            uint32_t span_mv = curve[i - 1].mv - curve[i].mv;
            uint32_t span_pct = curve[i - 1].pct - curve[i].pct;
            return curve[i].pct + ((mv - curve[i].mv) * span_pct) / span_mv;
        }
    }
    return 0;
}

static void measure_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(measure_work, measure_work_fn);

static void measure_work_fn(struct k_work* work) {
    int16_t sample = 0;
    const struct adc_sequence sequence = {
        .channels = BIT(0),
        .buffer = &sample,
        .buffer_size = sizeof(sample),
        .resolution = 12,
    };

    int err = adc_read(adc, &sequence);
    if (err) {
        LOG_ERR("adc_read failed: %d", err);
    } else {
        if (sample < 0) {
            sample = 0;
        }
        millivolts = ((uint32_t) sample * 3600) / 4096;
        percent = mv_to_percent(millivolts);
        bt_bas_set_battery_level(percent);
        LOG_INF("battery: %u mV, %u%%", millivolts, percent);
    }

    k_work_reschedule(&measure_work, MEASURE_INTERVAL);
}

void battery_init(void) {
    if (!device_is_ready(adc)) {
        LOG_ERR("ADC not ready");
        return;
    }

    int err = adc_channel_setup(adc, &channel_cfg);
    if (err) {
        LOG_ERR("adc_channel_setup failed: %d", err);
        return;
    }

    k_work_reschedule(&measure_work, K_SECONDS(2));
}

uint16_t battery_millivolts(void) {
    return millivolts;
}

uint8_t battery_percent(void) {
    return percent;
}
