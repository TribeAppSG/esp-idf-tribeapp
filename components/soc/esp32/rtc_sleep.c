// Copyright 2015-2017 Espressif Systems (Shanghai) PTE LTD
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

#include <stdint.h>
#include "soc/soc.h"
#include "soc/rtc.h"
#include "soc/dport_reg.h"
#include "soc/rtc.h"
#include "soc/i2s_periph.h"
#include "soc/timer_periph.h"
#include "soc/bb_reg.h"
#include "soc/nrx_reg.h"
#include "soc/fe_reg.h"
#include "soc/rtc.h"
#include "esp32/rom/ets_sys.h"

#define MHZ (1000000)

/* Various delays to be programmed into power control state machines */
#define RTC_CNTL_XTL_BUF_WAIT_SLP   2
#define RTC_CNTL_PLL_BUF_WAIT_SLP   2
#define RTC_CNTL_CK8M_WAIT_SLP      4
#define OTHER_BLOCKS_POWERUP        1
#define OTHER_BLOCKS_WAIT           1

#define ROM_RAM_POWERUP_CYCLES   OTHER_BLOCKS_POWERUP
#define ROM_RAM_WAIT_CYCLES      OTHER_BLOCKS_WAIT

#define WIFI_POWERUP_CYCLES      OTHER_BLOCKS_POWERUP
#define WIFI_WAIT_CYCLES         OTHER_BLOCKS_WAIT

#define RTC_POWERUP_CYCLES       OTHER_BLOCKS_POWERUP
#define RTC_WAIT_CYCLES          OTHER_BLOCKS_WAIT

#define DG_WRAP_POWERUP_CYCLES   OTHER_BLOCKS_POWERUP
#define DG_WRAP_WAIT_CYCLES      OTHER_BLOCKS_WAIT

#define RTC_MEM_POWERUP_CYCLES   OTHER_BLOCKS_POWERUP
#define RTC_MEM_WAIT_CYCLES      OTHER_BLOCKS_WAIT

/**
 * @brief Power down flags for rtc_sleep_pd function
 */
typedef struct {
    uint32_t dig_pd : 1;    //!< Set to 1 to power down digital part in sleep
    uint32_t rtc_pd : 1;    //!< Set to 1 to power down RTC memories in sleep
    uint32_t cpu_pd : 1;    //!< Set to 1 to power down digital memories and CPU in sleep
    uint32_t i2s_pd : 1;    //!< Set to 1 to power down I2S in sleep
    uint32_t bb_pd : 1;     //!< Set to 1 to power down WiFi in sleep
    uint32_t nrx_pd : 1;    //!< Set to 1 to power down WiFi in sleep
    uint32_t fe_pd : 1;     //!< Set to 1 to power down WiFi in sleep
} rtc_sleep_pd_config_t;

/**
 * Initializer for rtc_sleep_pd_config_t which sets all flags to the same value
 */
#define RTC_SLEEP_PD_CONFIG_ALL(val) {\
    .dig_pd = (val), \
    .rtc_pd = (val), \
    .cpu_pd = (val), \
    .i2s_pd = (val), \
    .bb_pd = (val), \
    .nrx_pd = (val), \
    .fe_pd = (val), \
}

/**
 * Configure whether certain peripherals are powered down in deep sleep
 * @param cfg power down flags as rtc_sleep_pd_config_t structure
 */
static void rtc_sleep_pd(rtc_sleep_pd_config_t cfg)
{
    REG_SET_FIELD(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_LSLP_MEM_FORCE_PU, ~cfg.dig_pd);
    REG_SET_FIELD(RTC_CNTL_PWC_REG, RTC_CNTL_SLOWMEM_FORCE_LPU, ~cfg.rtc_pd);
    REG_SET_FIELD(RTC_CNTL_PWC_REG, RTC_CNTL_FASTMEM_FORCE_LPU, ~cfg.rtc_pd);
    DPORT_REG_SET_FIELD(DPORT_MEM_PD_MASK_REG, DPORT_LSLP_MEM_PD_MASK, ~cfg.cpu_pd);
    REG_SET_FIELD(I2S_PD_CONF_REG(0), I2S_PLC_MEM_FORCE_PU, ~cfg.i2s_pd);
    REG_SET_FIELD(I2S_PD_CONF_REG(0), I2S_FIFO_FORCE_PU, ~cfg.i2s_pd);
    REG_SET_FIELD(BBPD_CTRL, BB_FFT_FORCE_PU, ~cfg.bb_pd);
    REG_SET_FIELD(BBPD_CTRL, BB_DC_EST_FORCE_PU, ~cfg.bb_pd);
    REG_SET_FIELD(NRXPD_CTRL, NRX_RX_ROT_FORCE_PU, ~cfg.nrx_pd);
    REG_SET_FIELD(NRXPD_CTRL, NRX_VIT_FORCE_PU, ~cfg.nrx_pd);
    REG_SET_FIELD(NRXPD_CTRL, NRX_DEMAP_FORCE_PU, ~cfg.nrx_pd);
    REG_SET_FIELD(FE_GEN_CTRL, FE_IQ_EST_FORCE_PU, ~cfg.fe_pd);
    REG_SET_FIELD(FE2_TX_INTERP_CTRL, FE2_TX_INF_FORCE_PU, ~cfg.fe_pd);
}

void rtc_sleep_init(rtc_sleep_config_t cfg)
{
    // set 5 PWC state machine times to fit in main state machine time
    REG_SET_FIELD(RTC_CNTL_TIMER1_REG, RTC_CNTL_PLL_BUF_WAIT, RTC_CNTL_PLL_BUF_WAIT_SLP);
    REG_SET_FIELD(RTC_CNTL_TIMER1_REG, RTC_CNTL_XTL_BUF_WAIT, RTC_CNTL_XTL_BUF_WAIT_SLP);
    REG_SET_FIELD(RTC_CNTL_TIMER1_REG, RTC_CNTL_CK8M_WAIT, RTC_CNTL_CK8M_WAIT_SLP);

    // set shortest possible sleep time limit
    REG_SET_FIELD(RTC_CNTL_TIMER5_REG, RTC_CNTL_MIN_SLP_VAL, RTC_CNTL_MIN_SLP_VAL_MIN);

    // set rom&ram timer
    REG_SET_FIELD(RTC_CNTL_TIMER3_REG, RTC_CNTL_ROM_RAM_POWERUP_TIMER, ROM_RAM_POWERUP_CYCLES);
    REG_SET_FIELD(RTC_CNTL_TIMER3_REG, RTC_CNTL_ROM_RAM_WAIT_TIMER, ROM_RAM_WAIT_CYCLES);
    // set wifi timer
    REG_SET_FIELD(RTC_CNTL_TIMER3_REG, RTC_CNTL_WIFI_POWERUP_TIMER, WIFI_POWERUP_CYCLES);
    REG_SET_FIELD(RTC_CNTL_TIMER3_REG, RTC_CNTL_WIFI_WAIT_TIMER, WIFI_WAIT_CYCLES);
    // set rtc peri timer
    REG_SET_FIELD(RTC_CNTL_TIMER4_REG, RTC_CNTL_POWERUP_TIMER, RTC_POWERUP_CYCLES);
    REG_SET_FIELD(RTC_CNTL_TIMER4_REG, RTC_CNTL_WAIT_TIMER, RTC_WAIT_CYCLES);
    // set digital wrap timer
    REG_SET_FIELD(RTC_CNTL_TIMER4_REG, RTC_CNTL_DG_WRAP_POWERUP_TIMER, DG_WRAP_POWERUP_CYCLES);
    REG_SET_FIELD(RTC_CNTL_TIMER4_REG, RTC_CNTL_DG_WRAP_WAIT_TIMER, DG_WRAP_WAIT_CYCLES);
    // set rtc memory timer
    REG_SET_FIELD(RTC_CNTL_TIMER5_REG, RTC_CNTL_RTCMEM_POWERUP_TIMER, RTC_MEM_POWERUP_CYCLES);
    REG_SET_FIELD(RTC_CNTL_TIMER5_REG, RTC_CNTL_RTCMEM_WAIT_TIMER, RTC_MEM_WAIT_CYCLES);

    REG_SET_FIELD(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_LSLP_MEM_FORCE_PU, cfg.lslp_mem_inf_fpu);

    rtc_sleep_pd_config_t pd_cfg = RTC_SLEEP_PD_CONFIG_ALL(cfg.lslp_meminf_pd);
    rtc_sleep_pd(pd_cfg);

    if (cfg.rtc_mem_inf_fpu) {
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_MEM_FORCE_PU);
    } else {
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_MEM_FORCE_PU);
    }

    if (cfg.rtc_mem_inf_follow_cpu) {
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_MEM_FOLW_CPU);
    } else {
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_MEM_FOLW_CPU);
    }

    if (cfg.rtc_fastmem_pd_en) {
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_FASTMEM_PD_EN);
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_FASTMEM_FORCE_PU);
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_FASTMEM_FORCE_NOISO);
    } else {
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_FASTMEM_PD_EN);
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_FASTMEM_FORCE_PU);
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_FASTMEM_FORCE_NOISO);
    }

    if (cfg.rtc_slowmem_pd_en) {
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_SLOWMEM_PD_EN);
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_SLOWMEM_FORCE_PU);
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_SLOWMEM_FORCE_NOISO);
    } else {
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_SLOWMEM_PD_EN);
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_SLOWMEM_FORCE_PU);
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_SLOWMEM_FORCE_NOISO);
    }

    if (cfg.rtc_peri_pd_en) {
        SET_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_PD_EN);
    } else {
        CLEAR_PERI_REG_MASK(RTC_CNTL_PWC_REG, RTC_CNTL_PD_EN);
    }

    if (cfg.wifi_pd_en) {
        SET_PERI_REG_MASK(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_PD_EN);
    } else {
        CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_PD_EN);
    }

    if (cfg.rom_mem_pd_en) {
        SET_PERI_REG_MASK(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_CPU_ROM_RAM_PD_EN);
    } else {
        CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_CPU_ROM_RAM_PD_EN);
    }

    if (cfg.deep_slp) {
        CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG,
                RTC_CNTL_DG_PAD_FORCE_ISO | RTC_CNTL_DG_PAD_FORCE_NOISO);
        SET_PERI_REG_MASK(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_DG_WRAP_PD_EN);
        CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_PWC_REG,
                RTC_CNTL_DG_WRAP_FORCE_PU | RTC_CNTL_DG_WRAP_FORCE_PD);
        CLEAR_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_BIAS_FORCE_NOSLEEP);

        // Shut down parts of RTC which may have been left enabled by the wireless drivers
        CLEAR_PERI_REG_MASK(RTC_CNTL_ANA_CONF_REG,
                RTC_CNTL_CKGEN_I2C_PU | RTC_CNTL_PLL_I2C_PU |
                RTC_CNTL_RFRX_PBUS_PU | RTC_CNTL_TXRF_I2C_PU);
        CLEAR_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_BB_I2C_FORCE_PU);
    } else {
        CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_DG_WRAP_PD_EN);
        REG_SET_FIELD(RTC_CNTL_BIAS_CONF_REG, RTC_CNTL_DBG_ATTEN, 0);
    }

    REG_SET_FIELD(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_XTL_FORCE_PU, cfg.xtal_fpu);

    if (REG_GET_FIELD(RTC_CNTL_CLK_CONF_REG, RTC_CNTL_ANA_CLK_RTC_SEL) == RTC_SLOW_FREQ_8MD256) {
        REG_SET_BIT(RTC_CNTL_CLK_CONF_REG, RTC_CNTL_CK8M_FORCE_PU);
    } else {
        REG_CLR_BIT(RTC_CNTL_CLK_CONF_REG, RTC_CNTL_CK8M_FORCE_PU);
    }

    /* enable VDDSDIO control by state machine */
    REG_CLR_BIT(RTC_CNTL_SDIO_CONF_REG, RTC_CNTL_SDIO_FORCE);
    REG_SET_FIELD(RTC_CNTL_SDIO_CONF_REG, RTC_CNTL_SDIO_PD_EN, cfg.vddsdio_pd_en);

    REG_SET_FIELD(RTC_CNTL_REG, RTC_CNTL_DBIAS_SLP, cfg.rtc_dbias_slp);
    REG_SET_FIELD(RTC_CNTL_REG, RTC_CNTL_DBIAS_WAK, cfg.rtc_dbias_wak);
    REG_SET_FIELD(RTC_CNTL_REG, RTC_CNTL_DIG_DBIAS_WAK, cfg.dig_dbias_wak);
    REG_SET_FIELD(RTC_CNTL_REG, RTC_CNTL_DIG_DBIAS_SLP, cfg.dig_dbias_slp);
}

void rtc_sleep_set_wakeup_time(uint64_t t)
{
    WRITE_PERI_REG(RTC_CNTL_SLP_TIMER0_REG, t & UINT32_MAX);
    WRITE_PERI_REG(RTC_CNTL_SLP_TIMER1_REG, t >> 32);
}

uint32_t rtc_sleep_start(uint32_t wakeup_opt, uint32_t reject_opt)
{
    REG_SET_FIELD(RTC_CNTL_WAKEUP_STATE_REG, RTC_CNTL_WAKEUP_ENA, wakeup_opt);
    WRITE_PERI_REG(RTC_CNTL_SLP_REJECT_CONF_REG, reject_opt);

    /* Start entry into sleep mode */
    SET_PERI_REG_MASK(RTC_CNTL_STATE0_REG, RTC_CNTL_SLEEP_EN);

    while (GET_PERI_REG_MASK(RTC_CNTL_INT_RAW_REG,
            RTC_CNTL_SLP_REJECT_INT_RAW | RTC_CNTL_SLP_WAKEUP_INT_RAW) == 0) {
        ;
    }
    /* In deep sleep mode, we never get here */
    uint32_t reject = REG_GET_FIELD(RTC_CNTL_INT_RAW_REG, RTC_CNTL_SLP_REJECT_INT_RAW);
    SET_PERI_REG_MASK(RTC_CNTL_INT_CLR_REG,
            RTC_CNTL_SLP_REJECT_INT_CLR | RTC_CNTL_SLP_WAKEUP_INT_CLR);

    /* restore DBG_ATTEN to the default value */
    REG_SET_FIELD(RTC_CNTL_BIAS_CONF_REG, RTC_CNTL_DBG_ATTEN, RTC_CNTL_DBG_ATTEN_DEFAULT);
    return reject;
}
