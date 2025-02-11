// Copyright 2015-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "soc/soc_caps.h"
#ifndef SOC_CAN_SUPPORTED
#error CAN is not supported in this chip target
#endif

#include "freertos/FreeRTOS.h"
#include "esp_types.h"
#include "esp_intr_alloc.h"
#include "esp_err.h"
#include "gpio.h"
#include "soc/can_caps.h"
#include "hal/can_types.h"

/* -------------------- Default initializers and flags ---------------------- */
/** @cond */    //Doxy command to hide preprocessor definitions from docs
/**
 * @brief Initializer macro for general configuration structure.
 *
 * This initializer macros allows the TX GPIO, RX GPIO, and operating mode to be
 * configured. The other members of the general configuration structure are
 * assigned default values.
 */
#define CAN_GENERAL_CONFIG_DEFAULT(tx_io_num, rx_io_num, op_mode) {.mode = op_mode, .tx_io = tx_io_num, .rx_io = rx_io_num,       \
                                                                   .clkout_io = CAN_IO_UNUSED, .bus_off_io = CAN_IO_UNUSED,       \
                                                                   .tx_queue_len = 5, .rx_queue_len = 5,                          \
                                                                   .alerts_enabled = CAN_ALERT_NONE,  .clkout_divider = 0,        \
                                                                   .intr_flags = ESP_INTR_FLAG_LEVEL1}

/**
 * @brief   Alert flags
 *
 * The following flags represents the various kind of alerts available in
 * the CAN driver. These flags can be used when configuring/reconfiguring
 * alerts, or when calling can_read_alerts().
 *
 * @note    The CAN_ALERT_AND_LOG flag is not an actual alert, but will configure
 *          the CAN driver to log to UART when an enabled alert occurs.
 */
#define CAN_ALERT_TX_IDLE               0x0001      /**< Alert(1): No more messages to transmit */
#define CAN_ALERT_TX_SUCCESS            0x0002      /**< Alert(2): The previous transmission was successful */
#define CAN_ALERT_BELOW_ERR_WARN        0x0004      /**< Alert(4): Both error counters have dropped below error warning limit */
#define CAN_ALERT_ERR_ACTIVE            0x0008      /**< Alert(8): CAN controller has become error active */
#define CAN_ALERT_RECOVERY_IN_PROGRESS  0x0010      /**< Alert(16): CAN controller is undergoing bus recovery */
#define CAN_ALERT_BUS_RECOVERED         0x0020      /**< Alert(32): CAN controller has successfully completed bus recovery */
#define CAN_ALERT_ARB_LOST              0x0040      /**< Alert(64): The previous transmission lost arbitration */
#define CAN_ALERT_ABOVE_ERR_WARN        0x0080      /**< Alert(128): One of the error counters have exceeded the error warning limit */
#define CAN_ALERT_BUS_ERROR             0x0100      /**< Alert(256): A (Bit, Stuff, CRC, Form, ACK) error has occurred on the bus */
#define CAN_ALERT_TX_FAILED             0x0200      /**< Alert(512): The previous transmission has failed (for single shot transmission) */
#define CAN_ALERT_RX_QUEUE_FULL         0x0400      /**< Alert(1024): The RX queue is full causing a frame to be lost */
#define CAN_ALERT_ERR_PASS              0x0800      /**< Alert(2048): CAN controller has become error passive */
#define CAN_ALERT_BUS_OFF               0x1000      /**< Alert(4096): Bus-off condition occurred. CAN controller can no longer influence bus */
#define CAN_ALERT_ALL                   0x1FFF      /**< Bit mask to enable all alerts during configuration */
#define CAN_ALERT_NONE                  0x0000      /**< Bit mask to disable all alerts during configuration */
#define CAN_ALERT_AND_LOG               0x2000      /**< Bit mask to enable alerts to also be logged when they occur. Note that logging from the ISR is disabled if CONFIG_TWAI_ISR_IN_IRAM is enabled (see docs). */

/** @endcond */

#define CAN_IO_UNUSED                   ((gpio_num_t) -1)   /**< Marks GPIO as unused in CAN configuration */

/* ----------------------- Enum and Struct Definitions ---------------------- */

/**
 * @brief   CAN driver states
 */
typedef enum {
    CAN_STATE_STOPPED,              /**< Stopped state. The CAN controller will not participate in any CAN bus activities */
    CAN_STATE_RUNNING,              /**< Running state. The CAN controller can transmit and receive messages */
    CAN_STATE_BUS_OFF,              /**< Bus-off state. The CAN controller cannot participate in bus activities until it has recovered */
    CAN_STATE_RECOVERING,           /**< Recovering state. The CAN controller is undergoing bus recovery */
} can_state_t;

/**
 * @brief   Structure for general configuration of the CAN driver
 *
 * @note    Macro initializers are available for this structure
 */
typedef struct {
    can_mode_t mode;                /**< Mode of CAN controller */
    gpio_num_t tx_io;               /**< Transmit GPIO number */
    gpio_num_t rx_io;               /**< Receive GPIO number */
    gpio_num_t clkout_io;           /**< CLKOUT GPIO number (optional, set to -1 if unused) */
    gpio_num_t bus_off_io;          /**< Bus off indicator GPIO number (optional, set to -1 if unused) */
    uint32_t tx_queue_len;          /**< Number of messages TX queue can hold (set to 0 to disable TX Queue) */
    uint32_t rx_queue_len;          /**< Number of messages RX queue can hold */
    uint32_t alerts_enabled;        /**< Bit field of alerts to enable (see documentation) */
    uint32_t clkout_divider;        /**< CLKOUT divider. Can be 1 or any even number from 2 to 14 (optional, set to 0 if unused) */
    int intr_flags;                 /**< Interrupt flags to set the priority of the driver's ISR. Note that to use the ESP_INTR_FLAG_IRAM, the CONFIG_TWAI_ISR_IN_IRAM option should be enabled first. */
} can_general_config_t;

/**
 * @brief   Structure to store status information of CAN driver
 */
typedef struct {
    can_state_t state;              /**< Current state of CAN controller (Stopped/Running/Bus-Off/Recovery) */
    uint32_t msgs_to_tx;            /**< Number of messages queued for transmission or awaiting transmission completion */
    uint32_t msgs_to_rx;            /**< Number of messages in RX queue waiting to be read */
    uint32_t tx_error_counter;      /**< Current value of Transmit Error Counter */
    uint32_t rx_error_counter;      /**< Current value of Receive Error Counter */
    uint32_t tx_failed_count;       /**< Number of messages that failed transmissions */
    uint32_t rx_missed_count;       /**< Number of messages that were lost due to a full RX queue */
    uint32_t arb_lost_count;        /**< Number of instances arbitration was lost */
    uint32_t bus_error_count;       /**< Number of instances a bus error has occurred */
} can_status_info_t;

/* ----------------------------- Public API -------------------------------- */

/**
 * @brief   Install CAN driver
 *
 * This function installs the CAN driver using three configuration structures.
 * The required memory is allocated and the CAN driver is placed in the stopped
 * state after running this function.
 *
 * @param[in]   g_config    General configuration structure
 * @param[in]   t_config    Timing configuration structure
 * @param[in]   f_config    Filter configuration structure
 *
 * @note    Macro initializers are available for the configuration structures (see documentation)
 *
 * @note    To reinstall the CAN driver, call can_driver_uninstall() first
 *
 * @return
 *      - ESP_OK: Successfully installed CAN driver
 *      - ESP_ERR_INVALID_ARG: Arguments are invalid
 *      - ESP_ERR_NO_MEM: Insufficient memory
 *      - ESP_ERR_INVALID_STATE: Driver is already installed
 */
esp_err_t can_driver_install(const can_general_config_t *g_config, const can_timing_config_t *t_config, const can_filter_config_t *f_config);

/**
 * @brief   Uninstall the CAN driver
 *
 * This function uninstalls the CAN driver, freeing the memory utilized by the
 * driver. This function can only be called when the driver is in the stopped
 * state or the bus-off state.
 *
 * @warning The application must ensure that no tasks are blocked on TX/RX
 *          queues or alerts when this function is called.
 *
 * @return
 *      - ESP_OK: Successfully uninstalled CAN driver
 *      - ESP_ERR_INVALID_STATE: Driver is not in stopped/bus-off state, or is not installed
 */
esp_err_t can_driver_uninstall(void);

/**
 * @brief   Start the CAN driver
 *
 * This function starts the CAN driver, putting the CAN driver into the running
 * state. This allows the CAN driver to participate in CAN bus activities such
 * as transmitting/receiving messages. The RX queue is reset in this function,
 * clearing any unread messages. This function can only be called when the CAN
 * driver is in the stopped state.
 *
 * @return
 *      - ESP_OK: CAN driver is now running
 *      - ESP_ERR_INVALID_STATE: Driver is not in stopped state, or is not installed
 */
esp_err_t can_start(void);

/**
 * @brief   Stop the CAN driver
 *
 * This function stops the CAN driver, preventing any further message from being
 * transmitted or received until can_start() is called. Any messages in the TX
 * queue are cleared. Any messages in the RX queue should be read by the
 * application after this function is called. This function can only be called
 * when the CAN driver is in the running state.
 *
 * @warning A message currently being transmitted/received on the CAN bus will
 *          be ceased immediately. This may lead to other CAN nodes interpreting
 *          the unfinished message as an error.
 *
 * @return
 *      - ESP_OK: CAN driver is now Stopped
 *      - ESP_ERR_INVALID_STATE: Driver is not in running state, or is not installed
 */
esp_err_t can_stop(void);

/**
 * @brief   Transmit a CAN message
 *
 * This function queues a CAN message for transmission. Transmission will start
 * immediately if no other messages are queued for transmission. If the TX queue
 * is full, this function will block until more space becomes available or until
 * it timesout. If the TX queue is disabled (TX queue length = 0 in configuration),
 * this function will return immediately if another message is undergoing
 * transmission. This function can only be called when the CAN driver is in the
 * running state and cannot be called under Listen Only Mode.
 *
 * @param[in]   message         Message to transmit
 * @param[in]   ticks_to_wait   Number of FreeRTOS ticks to block on the TX queue
 *
 * @note    This function does not guarantee that the transmission is successful.
 *          The TX_SUCCESS/TX_FAILED alert can be enabled to alert the application
 *          upon the success/failure of a transmission.
 *
 * @note    The TX_IDLE alert can be used to alert the application when no other
 *          messages are awaiting transmission.
 *
 * @return
 *      - ESP_OK: Transmission successfully queued/initiated
 *      - ESP_ERR_INVALID_ARG: Arguments are invalid
 *      - ESP_ERR_TIMEOUT: Timed out waiting for space on TX queue
 *      - ESP_FAIL: TX queue is disabled and another message is currently transmitting
 *      - ESP_ERR_INVALID_STATE: CAN driver is not in running state, or is not installed
 *      - ESP_ERR_NOT_SUPPORTED: Listen Only Mode does not support transmissions
 */
esp_err_t can_transmit(const can_message_t *message, TickType_t ticks_to_wait);

/**
 * @brief   Receive a CAN message
 *
 * This function receives a message from the RX queue. The flags field of the
 * message structure will indicate the type of message received. This function
 * will block if there are no messages in the RX queue
 *
 * @param[out]  message         Received message
 * @param[in]   ticks_to_wait   Number of FreeRTOS ticks to block on RX queue
 *
 * @warning The flags field of the received message should be checked to determine
 *          if the received message contains any data bytes.
 *
 * @return
 *      - ESP_OK: Message successfully received from RX queue
 *      - ESP_ERR_TIMEOUT: Timed out waiting for message
 *      - ESP_ERR_INVALID_ARG: Arguments are invalid
 *      - ESP_ERR_INVALID_STATE: CAN driver is not installed
 */
esp_err_t can_receive(can_message_t *message, TickType_t ticks_to_wait);

/**
 * @brief   Read CAN driver alerts
 *
 * This function will read the alerts raised by the CAN driver. If no alert has
 * been when this function is called, this function will block until an alert
 * occurs or until it timeouts.
 *
 * @param[out]  alerts          Bit field of raised alerts (see documentation for alert flags)
 * @param[in]   ticks_to_wait   Number of FreeRTOS ticks to block for alert
 *
 * @note    Multiple alerts can be raised simultaneously. The application should
 *          check for all alerts that have been enabled.
 *
 * @return
 *      - ESP_OK: Alerts read
 *      - ESP_ERR_TIMEOUT: Timed out waiting for alerts
 *      - ESP_ERR_INVALID_ARG: Arguments are invalid
 *      - ESP_ERR_INVALID_STATE: CAN driver is not installed
 */
esp_err_t can_read_alerts(uint32_t *alerts, TickType_t ticks_to_wait);

/**
 * @brief   Reconfigure which alerts are enabled
 *
 * This function reconfigures which alerts are enabled. If there are alerts
 * which have not been read whilst reconfiguring, this function can read those
 * alerts.
 *
 * @param[in]   alerts_enabled  Bit field of alerts to enable (see documentation for alert flags)
 * @param[out]  current_alerts  Bit field of currently raised alerts. Set to NULL if unused
 *
 * @return
 *      - ESP_OK: Alerts reconfigured
 *      - ESP_ERR_INVALID_STATE: CAN driver is not installed
 */
esp_err_t can_reconfigure_alerts(uint32_t alerts_enabled, uint32_t *current_alerts);

/**
 * @brief   Start the bus recovery process
 *
 * This function initiates the bus recovery process when the CAN driver is in
 * the bus-off state. Once initiated, the CAN driver will enter the recovering
 * state and wait for 128 occurrences of the bus-free signal on the CAN bus
 * before returning to the stopped state. This function will reset the TX queue,
 * clearing any messages pending transmission.
 *
 * @note    The BUS_RECOVERED alert can be enabled to alert the application when
 *          the bus recovery process completes.
 *
 * @return
 *      - ESP_OK: Bus recovery started
 *      - ESP_ERR_INVALID_STATE: CAN driver is not in the bus-off state, or is not installed
 */
esp_err_t can_initiate_recovery(void);

/**
 * @brief   Get current status information of the CAN driver
 *
 * @param[out]  status_info     Status information
 *
 * @return
 *      - ESP_OK: Status information retrieved
 *      - ESP_ERR_INVALID_ARG: Arguments are invalid
 *      - ESP_ERR_INVALID_STATE: CAN driver is not installed
 */
esp_err_t can_get_status_info(can_status_info_t *status_info);

/**
 * @brief   Clear the transmit queue
 *
 * This function will clear the transmit queue of all messages.
 *
 * @note    The transmit queue is automatically cleared when can_stop() or
 *          can_initiate_recovery() is called.
 *
 * @return
 *      - ESP_OK: Transmit queue cleared
 *      - ESP_ERR_INVALID_STATE: CAN driver is not installed or TX queue is disabled
 */
esp_err_t can_clear_transmit_queue(void);

/**
 * @brief   Clear the receive queue
 *
 * This function will clear the receive queue of all messages.
 *
 * @note    The receive queue is automatically cleared when can_start() is
 *          called.
 *
 * @return
 *      - ESP_OK: Transmit queue cleared
 *      - ESP_ERR_INVALID_STATE: CAN driver is not installed
 */
esp_err_t can_clear_receive_queue(void);

#ifdef __cplusplus
}
#endif
