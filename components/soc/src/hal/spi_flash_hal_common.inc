// Copyright 2015-2019 Espressif Systems (Shanghai) PTE LTD
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
#include "hal/spi_flash_hal.h"
#include "string.h"
#include "hal/hal_defs.h"
#include "sdkconfig.h"

#define ADDRESS_MASK_24BIT 0xFFFFFF
#define COMPUTE_DUMMY_CYCLELEN(host, base)    ((base) + ((spi_flash_memspi_data_t *)(host)->driver_data)->extra_dummy)

static inline spi_dev_t *get_spi_dev(spi_flash_host_driver_t *host)
{
    return ((spi_flash_memspi_data_t *)host->driver_data)->spi;
}

void spi_flash_hal_poll_cmd_done(spi_flash_host_driver_t *host)
{
    while (!spi_flash_ll_cmd_is_done(get_spi_dev(host))) {
        //nop
    }
}

esp_err_t spi_flash_hal_device_config(spi_flash_host_driver_t *host)
{
    spi_flash_memspi_data_t *drv_data = (spi_flash_memspi_data_t *)host->driver_data;
    spi_dev_t *dev = get_spi_dev(host);
    spi_flash_ll_reset(dev);
    spi_flash_ll_set_cs_pin(dev, drv_data->cs_num);
    spi_flash_ll_set_clock(dev, &drv_data->clock_conf);
    spi_flash_ll_set_cs_setup(dev, 1);
    return ESP_OK;
}

esp_err_t spi_flash_hal_configure_host_io_mode(
    spi_flash_host_driver_t *host,
    uint32_t command,
    uint32_t addr_bitlen,
    int dummy_cyclelen_base,
    esp_flash_io_mode_t io_mode)
{
    spi_dev_t *dev = get_spi_dev(host);

    if (!SOC_SPI_PERIPH_SUPPORT_MULTILINE_MODE(dev) && io_mode > SPI_FLASH_FASTRD) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    spi_flash_ll_set_command8(dev, command);
    spi_flash_ll_set_addr_bitlen(dev, addr_bitlen);
    // Add dummy cycles to compensate for latency of GPIO matrix and external delay, if necessary...
    spi_flash_ll_set_dummy(dev, COMPUTE_DUMMY_CYCLELEN(host, dummy_cyclelen_base));
    //disable all data phases, enable them later if needed
    spi_flash_ll_set_miso_bitlen(dev, 0);
    spi_flash_ll_set_mosi_bitlen(dev, 0);
    spi_flash_ll_set_read_mode(dev, io_mode);
    return ESP_OK;
}

esp_err_t spi_flash_hal_common_command(spi_flash_host_driver_t *host, spi_flash_trans_t *trans)
{
    host->configure_host_io_mode(host, trans->command, trans->address_bitlen, 0, SPI_FLASH_FASTRD);

    spi_dev_t *dev = get_spi_dev(host);

    //disable dummy if no input phase
    if (trans->miso_len == 0) {
        spi_flash_ll_set_dummy(dev, 0);
    }

    spi_flash_ll_set_address(dev, (trans->address & ADDRESS_MASK_24BIT) << 8);

    spi_flash_ll_set_mosi_bitlen(dev, trans->mosi_len * 8);
    spi_flash_ll_set_buffer_data(dev, trans->mosi_data, trans->mosi_len);

    spi_flash_ll_set_miso_bitlen(dev, trans->miso_len * 8);
    spi_flash_ll_user_start(dev);
    host->poll_cmd_done(host);
    spi_flash_ll_get_buffer_data(dev, trans->miso_data, trans->miso_len);
    return ESP_OK;
}

esp_err_t spi_flash_hal_read(spi_flash_host_driver_t *host, void *buffer, uint32_t address, uint32_t read_len)
{
    spi_dev_t *dev = get_spi_dev(host);
    spi_flash_ll_set_address(dev, address << 8);
    spi_flash_ll_set_miso_bitlen(dev, read_len * 8);
    spi_flash_ll_user_start(dev);
    host->poll_cmd_done(host);
    spi_flash_ll_get_buffer_data(dev, buffer, read_len);
    return ESP_OK;
}
