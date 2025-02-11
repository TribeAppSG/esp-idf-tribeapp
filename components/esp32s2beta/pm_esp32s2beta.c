// Copyright 2016-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp32s2beta/clk.h"
#include "esp_private/crosscore_int.h"

#include "soc/rtc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/xtensa_timer.h"
#include "xtensa/core-macros.h"

#include "esp_private/pm_impl.h"
#include "esp_private/pm_trace.h"
#include "esp_private/esp_timer_impl.h"
#include "esp32s2beta/pm.h"

/* CCOMPARE update timeout, in CPU cycles. Any value above ~600 cycles will work
 * for the purpose of detecting a deadlock.
 */
#define CCOMPARE_UPDATE_TIMEOUT 1000000

/* When changing CCOMPARE, don't allow changes if the difference is less
 * than this. This is to prevent setting CCOMPARE below CCOUNT.
 */
#define CCOMPARE_MIN_CYCLES_IN_FUTURE 1000

/* When light sleep is used, wake this number of microseconds earlier than
 * the next tick.
 */
#define LIGHT_SLEEP_EARLY_WAKEUP_US 100

#ifdef CONFIG_PM_PROFILING
#define WITH_PROFILING
#endif


static portMUX_TYPE s_switch_lock = portMUX_INITIALIZER_UNLOCKED;
/* The following state variables are protected using s_switch_lock: */
/* Current sleep mode; When switching, contains old mode until switch is complete */
static pm_mode_t s_mode = PM_MODE_CPU_MAX;
/* True when switch is in progress */
static volatile bool s_is_switching;
/* Number of times each mode was locked */
static size_t s_mode_lock_counts[PM_MODE_COUNT];
/* Bit mask of locked modes. BIT(i) is set iff s_mode_lock_counts[i] > 0. */
static uint32_t s_mode_mask;

/* Divider and multiplier used to adjust (ccompare - ccount) duration.
 * Only set to non-zero values when switch is in progress.
 */
static uint32_t s_ccount_div;
static uint32_t s_ccount_mul;

/* Indicates to the ISR hook that CCOMPARE needs to be updated on the given CPU.
 * Used in conjunction with cross-core interrupt to update CCOMPARE on the other CPU.
 */
static volatile bool s_need_update_ccompare[portNUM_PROCESSORS];

/* When no RTOS tasks are active, these locks are released to allow going into
 * a lower power mode. Used by ISR hook and idle hook.
 */
static esp_pm_lock_handle_t s_rtos_lock_handle[portNUM_PROCESSORS];

/* A flag indicating that Idle hook has run on a given CPU;
 * Next interrupt on the same CPU will take s_rtos_lock_handle.
 */
static bool s_core_idle[portNUM_PROCESSORS];

/* g_ticks_us defined in ROM for PRO CPU */
extern uint32_t g_ticks_per_us_pro;

/* Lookup table of CPU frequencies to be used in each mode.
 * Initialized by esp_pm_impl_init and modified by esp_pm_configure.
 */
rtc_cpu_freq_t s_cpu_freq_by_mode[PM_MODE_COUNT];

/* Lookup table of CPU ticks per microsecond for each RTC_CPU_FREQ_ value.
 * Essentially the same as returned by rtc_clk_cpu_freq_value(), but without
 * the function call. Not const because XTAL frequency is only known at run time.
 */
static uint32_t s_cpu_freq_to_ticks[] = {
       [RTC_CPU_FREQ_XTAL] = 0, /* This is set by esp_pm_impl_init */
       [RTC_CPU_FREQ_80M]  = 80,
       [RTC_CPU_FREQ_160M] = 160,
       [RTC_CPU_FREQ_240M] = 240,
       [RTC_CPU_FREQ_2M]   = 2
};

/* Lookup table of names for each RTC_CPU_FREQ_ value. Used for logging only. */
static const char* s_freq_names[] __attribute__((unused)) = {
        [RTC_CPU_FREQ_XTAL] = "XTAL",
        [RTC_CPU_FREQ_80M] = "80",
        [RTC_CPU_FREQ_160M] = "160",
        [RTC_CPU_FREQ_240M] = "240",
        [RTC_CPU_FREQ_2M] = "2"
};

/* Whether automatic light sleep is enabled */
static bool s_light_sleep_en = false;

/* When configuration is changed, current frequency may not match the
 * newly configured frequency for the current mode. This is an indicator
 * to the mode switch code to get the actual current frequency instead of
 * relying on the current mode.
 */
static bool s_config_changed = false;

#ifdef WITH_PROFILING
/* Time, in microseconds, spent so far in each mode */
static pm_time_t s_time_in_mode[PM_MODE_COUNT];
/* Timestamp, in microseconds, when the mode switch last happened */
static pm_time_t s_last_mode_change_time;
/* User-readable mode names, used by esp_pm_impl_dump_stats */
static const char* s_mode_names[] = {
        "SLEEP",
        "APB_MIN",
        "APB_MAX",
        "CPU_MAX"
};
#endif // WITH_PROFILING


static const char* TAG = "pm_esp32";

static void update_ccompare(void);
static void do_switch(pm_mode_t new_mode);
static void leave_idle(void);
static void on_freq_update(uint32_t old_ticks_per_us, uint32_t ticks_per_us);


pm_mode_t esp_pm_impl_get_mode(esp_pm_lock_type_t type, int arg)
{
    (void) arg;
    if (type == ESP_PM_CPU_FREQ_MAX) {
        return PM_MODE_CPU_MAX;
    } else if (type == ESP_PM_APB_FREQ_MAX) {
        return PM_MODE_APB_MAX;
    } else if (type == ESP_PM_NO_LIGHT_SLEEP) {
        return PM_MODE_APB_MIN;
    } else {
        // unsupported mode
        abort();
    }
}

/* rtc_cpu_freq_t enum is not ordered by frequency, so convert to MHz,
 * figure out the maximum value, then convert back to rtc_cpu_freq_t.
 */
static rtc_cpu_freq_t max_freq_of(rtc_cpu_freq_t f1, rtc_cpu_freq_t f2)
{
    int f1_hz = rtc_clk_cpu_freq_value(f1);
    int f2_hz = rtc_clk_cpu_freq_value(f2);
    int f_max_hz = MAX(f1_hz, f2_hz);
    rtc_cpu_freq_t result = RTC_CPU_FREQ_XTAL;
    if (!rtc_clk_cpu_freq_from_mhz(f_max_hz/1000000, &result)) {
        assert(false && "unsupported frequency");
    }
    return result;
}

esp_err_t esp_pm_configure(const void* vconfig)
{
#ifndef CONFIG_PM_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#endif

    const esp_pm_config_esp32_t* config = (const esp_pm_config_esp32_t*) vconfig;
#ifndef CONFIG_FREERTOS_USE_TICKLESS_IDLE
    if (config->light_sleep_enable) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    if (config->min_cpu_freq == RTC_CPU_FREQ_2M) {
        /* Minimal APB frequency to achieve 1MHz REF_TICK frequency is 5 MHz */
        return ESP_ERR_NOT_SUPPORTED;
    }

    rtc_cpu_freq_t min_freq = config->min_cpu_freq;
    rtc_cpu_freq_t max_freq = config->max_cpu_freq;
    int min_freq_mhz = rtc_clk_cpu_freq_value(min_freq);
    int max_freq_mhz = rtc_clk_cpu_freq_value(max_freq);
    if (min_freq_mhz > max_freq_mhz) {
        return ESP_ERR_INVALID_ARG;
    }

    rtc_cpu_freq_t apb_max_freq = max_freq; /* CPU frequency in APB_MAX mode */
    if (max_freq == RTC_CPU_FREQ_240M) {
        /* We can't switch between 240 and 80/160 without disabling PLL,
         * so use 240MHz CPU frequency when 80MHz APB frequency is requested.
         */
        apb_max_freq = RTC_CPU_FREQ_240M;
    } else if (max_freq == RTC_CPU_FREQ_160M || max_freq == RTC_CPU_FREQ_80M) {
        /* Otherwise, can use 80MHz
         * CPU frequency when 80MHz APB frequency is requested.
         */
        apb_max_freq = RTC_CPU_FREQ_80M;
    }

    apb_max_freq = max_freq_of(apb_max_freq, min_freq);

    ESP_LOGI(TAG, "Frequency switching config: "
                  "CPU_MAX: %s, APB_MAX: %s, APB_MIN: %s, Light sleep: %s",
                  s_freq_names[max_freq],
                  s_freq_names[apb_max_freq],
                  s_freq_names[min_freq],
                  config->light_sleep_enable ? "ENABLED" : "DISABLED");

    portENTER_CRITICAL(&s_switch_lock);
    s_cpu_freq_by_mode[PM_MODE_CPU_MAX] = max_freq;
    s_cpu_freq_by_mode[PM_MODE_APB_MAX] = apb_max_freq;
    s_cpu_freq_by_mode[PM_MODE_APB_MIN] = min_freq;
    s_cpu_freq_by_mode[PM_MODE_LIGHT_SLEEP] = min_freq;
    s_light_sleep_en = config->light_sleep_enable;
    s_config_changed = true;
    portEXIT_CRITICAL(&s_switch_lock);

    return ESP_OK;
}

static pm_mode_t IRAM_ATTR get_lowest_allowed_mode(void)
{
    /* TODO: optimize using ffs/clz */
    if (s_mode_mask >= BIT(PM_MODE_CPU_MAX)) {
        return PM_MODE_CPU_MAX;
    } else if (s_mode_mask >= BIT(PM_MODE_APB_MAX)) {
        return PM_MODE_APB_MAX;
    } else if (s_mode_mask >= BIT(PM_MODE_APB_MIN) || !s_light_sleep_en) {
        return PM_MODE_APB_MIN;
    } else {
        return PM_MODE_LIGHT_SLEEP;
    }
}

void IRAM_ATTR esp_pm_impl_switch_mode(pm_mode_t mode,
        pm_mode_switch_t lock_or_unlock, pm_time_t now)
{
    bool need_switch = false;
    uint32_t mode_mask = BIT(mode);
    portENTER_CRITICAL(&s_switch_lock);
    uint32_t count;
    if (lock_or_unlock == MODE_LOCK) {
        count = ++s_mode_lock_counts[mode];
    } else {
        count = s_mode_lock_counts[mode]--;
    }
    if (count == 1) {
        if (lock_or_unlock == MODE_LOCK) {
            s_mode_mask |= mode_mask;
        } else {
            s_mode_mask &= ~mode_mask;
        }
        need_switch = true;
    }

    pm_mode_t new_mode = s_mode;
    if (need_switch) {
        new_mode = get_lowest_allowed_mode();
#ifdef WITH_PROFILING
        if (s_last_mode_change_time != 0) {
            pm_time_t diff = now - s_last_mode_change_time;
            s_time_in_mode[s_mode] += diff;
        }
        s_last_mode_change_time = now;
#endif // WITH_PROFILING
    }
    portEXIT_CRITICAL(&s_switch_lock);
    if (need_switch) {
        do_switch(new_mode);
    }
}

/**
 * @brief Update clock dividers in esp_timer and FreeRTOS, and adjust CCOMPARE
 * values on both CPUs.
 * @param old_ticks_per_us old CPU frequency
 * @param ticks_per_us new CPU frequency
 */
static void IRAM_ATTR on_freq_update(uint32_t old_ticks_per_us, uint32_t ticks_per_us)
{
    uint32_t old_apb_ticks_per_us = MIN(old_ticks_per_us, 80);
    uint32_t apb_ticks_per_us = MIN(ticks_per_us, 80);
    /* Update APB frequency value used by the timer */
    if (old_apb_ticks_per_us != apb_ticks_per_us) {
        esp_timer_impl_update_apb_freq(apb_ticks_per_us);
    }

    /* Calculate new tick divisor */
    _xt_tick_divisor = ticks_per_us * 1000000 / XT_TICK_PER_SEC;

    int core_id = xPortGetCoreID();
    if (s_rtos_lock_handle[core_id] != NULL) {
        ESP_PM_TRACE_ENTER(CCOMPARE_UPDATE, core_id);
        /* ccount_div and ccount_mul are used in esp_pm_impl_update_ccompare
         * to calculate new CCOMPARE value.
         */
        s_ccount_div = old_ticks_per_us;
        s_ccount_mul = ticks_per_us;

        /* Update CCOMPARE value on this CPU */
        update_ccompare();

#if portNUM_PROCESSORS == 2
        /* Send interrupt to the other CPU to update CCOMPARE value */
        int other_core_id = (core_id == 0) ? 1 : 0;

        s_need_update_ccompare[other_core_id] = true;
        esp_crosscore_int_send_freq_switch(other_core_id);

        int timeout = 0;
        while (s_need_update_ccompare[other_core_id]) {
            if (++timeout == CCOMPARE_UPDATE_TIMEOUT) {
                assert(false && "failed to update CCOMPARE, possible deadlock");
            }
        }
#endif // portNUM_PROCESSORS == 2

        s_ccount_mul = 0;
        s_ccount_div = 0;
        ESP_PM_TRACE_EXIT(CCOMPARE_UPDATE, core_id);
    }
}

/**
 * Perform the switch to new power mode.
 * Currently only changes the CPU frequency and adjusts clock dividers.
 * No light sleep yet.
 * @param new_mode mode to switch to
 */
static void IRAM_ATTR do_switch(pm_mode_t new_mode)
{
    const int core_id = xPortGetCoreID();

    do {
        portENTER_CRITICAL_ISR(&s_switch_lock);
        if (!s_is_switching) {
            break;
        }
        if (s_need_update_ccompare[core_id]) {
            s_need_update_ccompare[core_id] = false;
        }
        portEXIT_CRITICAL_ISR(&s_switch_lock);
    } while (true);
    if (new_mode == s_mode) {
        portEXIT_CRITICAL_ISR(&s_switch_lock);
        return;
    }
    s_is_switching = true;
    bool config_changed = s_config_changed;
    s_config_changed = false;
    portEXIT_CRITICAL_ISR(&s_switch_lock);

    rtc_cpu_freq_t new_freq = s_cpu_freq_by_mode[new_mode];
    rtc_cpu_freq_t old_freq;
    if (!config_changed) {
        old_freq = s_cpu_freq_by_mode[s_mode];
    } else {
        old_freq = rtc_clk_cpu_freq_get();
    }

    if (new_freq != old_freq) {
        uint32_t old_ticks_per_us = g_ticks_per_us_pro;
        uint32_t new_ticks_per_us = s_cpu_freq_to_ticks[new_freq];

        bool switch_down = new_ticks_per_us < old_ticks_per_us;

        ESP_PM_TRACE_ENTER(FREQ_SWITCH, core_id);
        if (switch_down) {
            on_freq_update(old_ticks_per_us, new_ticks_per_us);
        }
        rtc_clk_cpu_freq_set_fast(new_freq);
        if (!switch_down) {
            on_freq_update(old_ticks_per_us, new_ticks_per_us);
        }
        ESP_PM_TRACE_EXIT(FREQ_SWITCH, core_id);
    }

    portENTER_CRITICAL_ISR(&s_switch_lock);
    s_mode = new_mode;
    s_is_switching = false;
    portEXIT_CRITICAL_ISR(&s_switch_lock);
}

/**
 * @brief Calculate new CCOMPARE value based on s_ccount_{mul,div}
 *
 * Adjusts CCOMPARE value so that the interrupt happens at the same time as it
 * would happen without the frequency change.
 * Assumes that the new_frequency = old_frequency * s_ccount_mul / s_ccount_div.
 */
static void IRAM_ATTR update_ccompare(void)
{
    uint32_t ccount = XTHAL_GET_CCOUNT();
    uint32_t ccompare = XTHAL_GET_CCOMPARE(XT_TIMER_INDEX);
    if ((ccompare - CCOMPARE_MIN_CYCLES_IN_FUTURE) - ccount < UINT32_MAX / 2) {
        uint32_t diff = ccompare - ccount;
        uint32_t diff_scaled = (diff * s_ccount_mul + s_ccount_div - 1) / s_ccount_div;
        if (diff_scaled < _xt_tick_divisor) {
            uint32_t new_ccompare = ccount + diff_scaled;
            XTHAL_SET_CCOMPARE(XT_TIMER_INDEX, new_ccompare);
        }
    }
}

static void IRAM_ATTR leave_idle(void)
{
    int core_id = xPortGetCoreID();
    if (s_core_idle[core_id]) {
        // TODO: possible optimization: raise frequency here first
        esp_pm_lock_acquire(s_rtos_lock_handle[core_id]);
        s_core_idle[core_id] = false;
    }
}

void esp_pm_impl_idle_hook(void)
{
    int core_id = xPortGetCoreID();
    uint32_t state = portENTER_CRITICAL_NESTED();
    if (!s_core_idle[core_id]) {
        esp_pm_lock_release(s_rtos_lock_handle[core_id]);
        s_core_idle[core_id] = true;
    }
    portEXIT_CRITICAL_NESTED(state);
    ESP_PM_TRACE_ENTER(IDLE, core_id);
}

void esp_pm_impl_waiti(void)
{
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    int core_id = xPortGetCoreID();
    if (s_skipped_light_sleep[core_id]) {
        asm("waiti 0");
        /* Interrupt took the CPU out of waiti and s_rtos_lock_handle[core_id]
         * is now taken. However since we are back to idle task, we can release
         * the lock so that vApplicationSleep can attempt to enter light sleep.
         */
        esp_pm_impl_idle_hook();
    }
    s_skipped_light_sleep[core_id] = true;
#else
    asm("waiti 0");
#endif // CONFIG_FREERTOS_USE_TICKLESS_IDLE
}

void IRAM_ATTR esp_pm_impl_isr_hook(void)
{
    int core_id = xPortGetCoreID();
    ESP_PM_TRACE_ENTER(ISR_HOOK, core_id);
#if portNUM_PROCESSORS == 2
    if (s_need_update_ccompare[core_id]) {
        update_ccompare();
        s_need_update_ccompare[core_id] = false;
    } else {
        leave_idle();
    }
#else
    leave_idle();
#endif // portNUM_PROCESSORS == 2
    ESP_PM_TRACE_EXIT(ISR_HOOK, core_id);
}

#if CONFIG_FREERTOS_USE_TICKLESS_IDLE

bool IRAM_ATTR vApplicationSleep( TickType_t xExpectedIdleTime )
{
    bool result = false;
    portENTER_CRITICAL(&s_switch_lock);
    if (s_mode == PM_MODE_LIGHT_SLEEP && !s_is_switching) {
        /* Calculate how much we can sleep */
        int64_t next_esp_timer_alarm = esp_timer_get_next_alarm();
        int64_t now = esp_timer_get_time();
        int64_t time_until_next_alarm = next_esp_timer_alarm - now;
        int64_t wakeup_delay_us = portTICK_PERIOD_MS * 1000LL * xExpectedIdleTime;
        int64_t sleep_time_us = MIN(wakeup_delay_us, time_until_next_alarm);
        if (sleep_time_us >= configEXPECTED_IDLE_TIME_BEFORE_SLEEP * portTICK_PERIOD_MS * 1000LL) {
            esp_sleep_enable_timer_wakeup(sleep_time_us - LIGHT_SLEEP_EARLY_WAKEUP_US);
#ifdef CONFIG_PM_TRACE
            /* to force tracing GPIOs to keep state */
            esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif
            /* Enter sleep */
            int core_id = xPortGetCoreID();
            ESP_PM_TRACE_ENTER(SLEEP, core_id);
            int64_t sleep_start = esp_timer_get_time();
            esp_light_sleep_start();
            int64_t slept_us = esp_timer_get_time() - sleep_start;
            ESP_PM_TRACE_EXIT(SLEEP, core_id);

            uint32_t slept_ticks = slept_us / (portTICK_PERIOD_MS * 1000LL);
            if (slept_ticks > 0) {
                /* Adjust RTOS tick count based on the amount of time spent in sleep */
                vTaskStepTick(slept_ticks);

                /* Trigger tick interrupt, since sleep time was longer
                 * than portTICK_PERIOD_MS. Note that setting INTSET does not
                 * work for timer interrupt, and changing CCOMPARE would clear
                 * the interrupt flag.
                 */
                XTHAL_SET_CCOUNT(XTHAL_GET_CCOMPARE(XT_TIMER_INDEX) - 16);
                while (!(XTHAL_GET_INTERRUPT() & BIT(XT_TIMER_INTNUM))) {
                    ;
                }
            }
            result = true;
        }
    }
    portEXIT_CRITICAL(&s_switch_lock);
    return result;
}
#endif //CONFIG_FREERTOS_USE_TICKLESS_IDLE

#ifdef WITH_PROFILING
void esp_pm_impl_dump_stats(FILE* out)
{
    pm_time_t time_in_mode[PM_MODE_COUNT];

    portENTER_CRITICAL_ISR(&s_switch_lock);
    memcpy(time_in_mode, s_time_in_mode, sizeof(time_in_mode));
    pm_time_t last_mode_change_time = s_last_mode_change_time;
    pm_mode_t cur_mode = s_mode;
    pm_time_t now = pm_get_time();
    portEXIT_CRITICAL_ISR(&s_switch_lock);

    time_in_mode[cur_mode] += now - last_mode_change_time;

    fprintf(out, "Mode stats:\n");
    for (int i = 0; i < PM_MODE_COUNT; ++i) {
        if (i == PM_MODE_LIGHT_SLEEP && !s_light_sleep_en) {
            /* don't display light sleep mode if it's not enabled */
            continue;
        }
        fprintf(out, "%8s %6s %12lld  %2d%%\n",
                s_mode_names[i],
                s_freq_names[s_cpu_freq_by_mode[i]],
                time_in_mode[i],
                (int) (time_in_mode[i] * 100 / now));
    }
}
#endif // WITH_PROFILING

void esp_pm_impl_init(void)
{
    s_cpu_freq_to_ticks[RTC_CPU_FREQ_XTAL] = rtc_clk_xtal_freq_get();
#ifdef CONFIG_PM_TRACE
    esp_pm_trace_init();
#endif
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "rtos0",
            &s_rtos_lock_handle[0]));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(s_rtos_lock_handle[0]));
#if portNUM_PROCESSORS == 2
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "rtos1",
            &s_rtos_lock_handle[1]));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(s_rtos_lock_handle[1]));
#endif // portNUM_PROCESSORS == 2

    /* Configure all modes to use the default CPU frequency.
     * This will be modified later by a call to esp_pm_configure.
     */
    rtc_cpu_freq_t default_freq;
    if (!rtc_clk_cpu_freq_from_mhz(CONFIG_ESP32S2_DEFAULT_CPU_FREQ_MHZ, &default_freq)) {
        assert(false && "unsupported frequency");
    }
    for (size_t i = 0; i < PM_MODE_COUNT; ++i) {
        s_cpu_freq_by_mode[i] = default_freq;
    }
}
