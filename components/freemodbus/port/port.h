/* Copyright 2018 Espressif Systems (Shanghai) PTE LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PORT_COMMON_H_
#define PORT_COMMON_H_

#include "freertos/FreeRTOS.h"
#include "freertos/xtensa_api.h"
#include "freertos/portmacro.h"
#include "esp_log.h"                // for ESP_LOGE macro

#define INLINE                      inline
#define PR_BEGIN_EXTERN_C           extern "C" {
#define PR_END_EXTERN_C             }

#define MB_PORT_TAG "MB_PORT_COMMON"

// common definitions for serial port implementations
#define MB_SERIAL_TX_TOUT_MS        (2200) // maximum time for transmission of longest allowed frame buffer
#define MB_SERIAL_TX_TOUT_TICKS     pdMS_TO_TICKS(MB_SERIAL_TX_TOUT_MS) // timeout for transmission
#define MB_SERIAL_RX_TOUT_TICKS     pdMS_TO_TICKS(1) // timeout for rx from buffer
#define MB_SERIAL_RESP_LEN_MIN      (4)

// The task affinity for Modbus stack tasks
#define MB_PORT_TASK_AFFINITY           (CONFIG_FMB_PORT_TASK_AFFINITY)

#define MB_PORT_CHECK(a, ret_val, str, ...) \
    if (!(a)) { \
        ESP_LOGE(MB_PORT_TAG, "%s(%u): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        return ret_val; \
    }

#ifdef __cplusplus
PR_BEGIN_EXTERN_C
#endif /* __cplusplus */

#ifndef TRUE
#define TRUE            1
#endif

#ifndef FALSE
#define FALSE           0
#endif

typedef char    BOOL;

typedef unsigned char UCHAR;
typedef char    CHAR;

typedef unsigned short USHORT;
typedef short   SHORT;

typedef unsigned long ULONG;
typedef long    LONG;

void vMBPortEnterCritical(void);
void vMBPortExitCritical(void);

#define ENTER_CRITICAL_SECTION( ) { ESP_LOGD(MB_PORT_TAG,"%s: Port enter critical.", __func__); \
                                    vMBPortEnterCritical(); }

#define EXIT_CRITICAL_SECTION( )  { vMBPortExitCritical(); \
                                    ESP_LOGD(MB_PORT_TAG,"%s: Port exit critical", __func__); }

#define MB_PORT_CHECK_EVENT( event, mask ) ( event & mask )
#define MB_PORT_CLEAR_EVENT( event, mask ) do { event &= ~mask; } while(0)

#define MB_PORT_PARITY_GET(parity) ((parity != UART_PARITY_DISABLE) ? \
                                        ((parity == UART_PARITY_ODD) ? MB_PAR_ODD : MB_PAR_EVEN) : MB_PAR_NONE)
#ifdef __cplusplus
PR_END_EXTERN_C
#endif /* __cplusplus */

#endif /* PORT_COMMON_H_ */
