// Copyright 2019 Espressif Systems (Shanghai) PTE LTD
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

#include "lwip/dns.h"
#include "esp_netif.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_event.h"
#include "esp_netif_ppp.h"
#include "esp_netif_lwip_internal.h"
#include "lwip/ip6_addr.h"
#include <string.h>

ESP_EVENT_DEFINE_BASE(NETIF_PPP_STATUS);

static const char *TAG = "esp-netif_lwip-ppp";

#if PPPOS_SUPPORT

/**
 * @brief internal lwip_ppp context struct, used to hold PPP netif related parameters
 */
struct lwip_ppp_ctx {
    bool ppp_phase_event_enabled;
    bool ppp_error_event_enabled;
    ppp_pcb *ppp;
};

/**
 * @brief lwip callback from PPP client used here to produce PPP error related events,
 * as well as some IP events
 */
static void on_ppp_status_changed(ppp_pcb *pcb, int err_code, void *ctx)
{
    struct netif *pppif = ppp_netif(pcb);
    const ip_addr_t *dest_ip = NULL;
    esp_netif_t *netif = ctx;
    ip_event_got_ip_t evt = {
            .esp_netif = netif,
            .if_index = -1,
    };
    esp_err_t err;
    struct lwip_ppp_ctx *obj =  netif->lwip_ppp_ctx;
    esp_ip4_addr_t ns1;
    esp_ip4_addr_t ns2;
    switch (err_code) {
        case PPPERR_NONE: /* Connected */
            ESP_LOGI(TAG, "Connected");
            if (pcb->if4_up && !ip_addr_isany(&pppif->ip_addr)) {
                esp_netif_ip_info_t *ip_info = netif->ip_info;
                ip4_addr_set(&ip_info->ip, ip_2_ip4(&pppif->ip_addr));
                ip4_addr_set(&ip_info->netmask, ip_2_ip4(&pppif->netmask));
                ip4_addr_set(&ip_info->gw, ip_2_ip4(&pppif->gw));

                evt.ip_info.ip.addr = pppif->ip_addr.u_addr.ip4.addr;
                evt.ip_info.gw.addr = pppif->gw.u_addr.ip4.addr;
                evt.ip_info.netmask.addr = pppif->netmask.u_addr.ip4.addr;

                dest_ip = dns_getserver(0);
                if(dest_ip != NULL){
                    ns1.addr = (*dest_ip).u_addr.ip4.addr;
                }
                dest_ip = dns_getserver(1);
                if(dest_ip != NULL){
                    ns2.addr = (*dest_ip).u_addr.ip4.addr;
                }
                ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&ns1));
                ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&ns2));


                err = esp_event_post(IP_EVENT, netif->get_ip_event, &evt, sizeof(evt), 0);
                if (ESP_OK != err) {
                    ESP_LOGE(TAG, "esp_event_post failed with code %d", err);
                }
                return;
#if PPP_IPV6_SUPPORT
            } else if (pcb->if6_up && !ip_addr_isany(&pppif->ip6_addr[0])) {
                esp_netif_ip6_info_t ip6_info;
                ip6_addr_t lwip_ip6_info;
                ip_event_got_ip6_t ip6_event = { .esp_netif = pppif->state, .if_index = -1 };

                ip6_addr_set(&lwip_ip6_info, ip_2_ip6(&pppif->ip6_addr[0]));
#if LWIP_IPV6_SCOPES
                memcpy(&ip6_info.ip, &lwip_ip6_info, sizeof(esp_ip6_addr_t));
#else
                memcpy(&ip6_info.ip, &lwip_ip6_info, sizeof(ip6_addr_t));
                ip6_info.ip.zone = 0;   // zero out zone, as not used in lwip
#endif /* LWIP_IPV6_SCOPES */
                memcpy(&ip6_event.ip6_info, &ip6_info, sizeof(esp_netif_ip6_info_t));

                ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(pppif->ip6_addr[0].u_addr.ip6));
                err = esp_event_post(IP_EVENT, IP_EVENT_GOT_IP6, &ip6_event, sizeof(ip6_event), 0);
                if (ESP_OK != err) {
                    ESP_LOGE(TAG, "esp_event_post failed with code %d", err);
                }
                return;
#endif /* PPP_IPV6_SUPPORT */
            } else {
                ESP_LOGE(TAG, "Unexpected connected event");
                return;
            }

        case PPPERR_PARAM:
            ESP_LOGE(TAG, "Invalid parameter");
            break;
        case PPPERR_OPEN:
            ESP_LOGE(TAG, "Unable to open PPP session");
            break;
        case PPPERR_DEVICE:
            ESP_LOGE(TAG, "Invalid I/O device for PPP");
            break;
        case PPPERR_ALLOC:
            ESP_LOGE(TAG, "Unable to allocate resources");
            break;
        case PPPERR_USER: /* User interrupt */
            ESP_LOGI(TAG, "User interrupt");
            break;
        case PPPERR_CONNECT: /* Connection lost */
            ESP_LOGI(TAG, "Connection lost");
            err = esp_event_post(IP_EVENT, netif->lost_ip_event, &evt, sizeof(evt), 0);

            if (ESP_OK != err) {
                ESP_LOGE(TAG, "esp_event_send_internal failed with code %d", err);
            }
            return;

        case PPPERR_AUTHFAIL:
            ESP_LOGE(TAG, "Failed authentication challenge");
            break;
        case PPPERR_PROTOCOL:
            ESP_LOGE(TAG, "Failed to meet protocol");
            break;
        case PPPERR_PEERDEAD:
            ESP_LOGE(TAG, "Connection timeout");
            break;
        case PPPERR_IDLETIMEOUT:
            ESP_LOGE(TAG, "Idle Timeout");
            break;
        case PPPERR_CONNECTTIME:
            ESP_LOGE(TAG, "Max connect time reached");
            break;
        case PPPERR_LOOPBACK:
            ESP_LOGE(TAG, "Loopback detected");
            break;
        default:
            ESP_LOGE(TAG, "Unknown error code %d", err_code);
            break;
    }
    if (obj->ppp_error_event_enabled) {
        err = esp_event_post(NETIF_PPP_STATUS, err_code, &netif, sizeof(netif), 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_event_post failed with code %d", err);
        }

    }
}

#if PPP_NOTIFY_PHASE
/**
 * @brief Notify phase callback which is called on each PPP internal state change
 *
 * @param pcb PPP control block
 * @param phase Phase ID
 * @param ctx Context of callback
 */
static void on_ppp_notify_phase(ppp_pcb *pcb, u8_t phase, void *ctx)
{
    switch (phase) {
        case PPP_PHASE_DEAD:
            ESP_LOGD(TAG, "Phase Dead");
            break;
        case PPP_PHASE_INITIALIZE:
            ESP_LOGD(TAG, "Phase Start");
            break;
        case PPP_PHASE_ESTABLISH:
            ESP_LOGD(TAG, "Phase Establish");
            break;
        case PPP_PHASE_AUTHENTICATE:
            ESP_LOGD(TAG, "Phase Authenticate");
            break;
        case PPP_PHASE_NETWORK:
            ESP_LOGD(TAG, "Phase Network");
            break;
        case PPP_PHASE_RUNNING:
            ESP_LOGD(TAG, "Phase Running");
            break;
        case PPP_PHASE_TERMINATE:
            ESP_LOGD(TAG, "Phase Terminate");
            break;
        case PPP_PHASE_DISCONNECT:
            ESP_LOGD(TAG, "Phase Disconnect");
            break;
        default:
            ESP_LOGW(TAG, "Phase Unknown: %d", phase);
            break;
    }
    esp_netif_t *netif = ctx;
    struct lwip_ppp_ctx *obj =  netif->lwip_ppp_ctx;
    if (obj && obj->ppp_phase_event_enabled) {
        esp_err_t err = esp_event_post(NETIF_PPP_STATUS, NETIF_PP_PHASE_OFFSET + phase, &netif, sizeof(netif), 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_event_post failed with code %d", err);
        }
    }
}
#endif // PPP_NOTIFY_PHASE

/**
 * @brief PPP low level output callback used to transmit data using standard esp-netif interafce
 *
 * @param pcb PPP control block
 * @param data Buffer to write to serial port
 * @param len Length of the data buffer
 * @param ctx Context of callback
 *
 * @return uint32_t Length of data successfully sent
 */
static uint32_t pppos_low_level_output(ppp_pcb *pcb, uint8_t *data, uint32_t len, void *netif)
{
    esp_err_t ret = esp_netif_transmit(netif, data, len);
    if (ret == ESP_OK) {
        return len;
    }
    return 0;
}

esp_err_t esp_netif_ppp_set_auth(esp_netif_t *netif, esp_netif_auth_type_t authtype, const char *user, const char *passwd)
{
    if (netif == NULL || !netif->is_ppp_netif) {
        return ESP_ERR_ESP_NETIF_INVALID_PARAMS;
    }
#if PPP_AUTH_SUPPORT
    struct lwip_ppp_ctx *obj =  netif->lwip_ppp_ctx;
    pppapi_set_auth(obj->ppp, authtype, user, passwd);
#else
#error "Unsupported AUTH Negotiation"
#endif
    return ESP_OK;
}

void esp_netif_ppp_set_default_netif(lwip_ppp_ctx_t* ppp_ctx)
{
    ppp_set_default(ppp_ctx->ppp);
}

lwip_ppp_ctx_t* esp_netif_new_ppp(esp_netif_t *esp_netif, const esp_netif_netstack_config_t *esp_netif_stack_config)
{
    struct netif * netif_impl = esp_netif->lwip_netif;
    struct lwip_ppp_ctx * ppp_obj = calloc(1, sizeof(struct lwip_ppp_ctx));
    if (ppp_obj == NULL) {
        ESP_LOGE(TAG, "%s: cannot allocate lwip_ppp_ctx", __func__);
        return NULL;
    }

    ppp_obj->ppp = pppapi_pppos_create(netif_impl, pppos_low_level_output, on_ppp_status_changed, esp_netif);
    ESP_LOGD(TAG, "%s: PPP connection created: %p", __func__, ppp_obj->ppp);
    if (!ppp_obj->ppp) {
        ESP_LOGE(TAG, "%s: lwIP PPP connection cannot be created", __func__);
    }
#if PPP_NOTIFY_PHASE
    ppp_set_notify_phase_callback(ppp_obj->ppp, on_ppp_notify_phase);
#endif
    ppp_set_usepeerdns(ppp_obj->ppp, 1);

    return ppp_obj;
}

esp_err_t esp_netif_start_ppp(lwip_ppp_ctx_t *ppp_ctx)
{
    ESP_LOGD(TAG, "%s: Starting PPP connection: %p", __func__, ppp_ctx->ppp);
    esp_err_t err = pppapi_connect(ppp_ctx->ppp, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: PPP connection cannot be started", __func__);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void esp_netif_lwip_ppp_input(void *ppp_ctx, void *buffer, size_t len, void *eb)
{
    struct lwip_ppp_ctx * obj = ppp_ctx;
    err_t ret = pppos_input_tcpip(obj->ppp, buffer, len);
    if (ret != ERR_OK) {
        ESP_LOGE(TAG, "pppos_input_tcpip failed with %d", ret);
    }
}

esp_err_t esp_netif_stop_ppp(lwip_ppp_ctx_t *ppp_ctx)
{
    ESP_LOGD(TAG, "%s: Stopped PPP connection: %p", __func__, ppp_ctx->ppp);
    err_t ret = pppapi_close(ppp_ctx->ppp, 0);
    if (ret != ERR_OK) {
        ESP_LOGE(TAG, "pppapi_close failed with %d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void esp_netif_destroy_ppp(lwip_ppp_ctx_t *ppp_ctx)
{
    pppapi_free(ppp_ctx->ppp);
    free(ppp_ctx);
}

esp_err_t esp_netif_ppp_set_params(esp_netif_t *netif, const esp_netif_ppp_config_t *config)
{
    struct lwip_ppp_ctx *obj =  netif->lwip_ppp_ctx;
    obj->ppp_phase_event_enabled = config->ppp_phase_event_enabled;
    obj->ppp_error_event_enabled = config->ppp_error_event_enabled;
    return ESP_OK;
}
#else  /* PPPOS_SUPPORT */

/**
 * @brief If PPP not enabled in menuconfig, log the error and return appropriate code indicating failure
*/
#define LOG_PPP_DISABLED_AND_DO(action) \
    {   \
    ESP_LOGE(TAG, "%s not supported, please enable PPP in lwIP component configuration", __func__); \
    action; \
    }

esp_err_t esp_netif_ppp_set_auth(esp_netif_t *netif, esp_netif_auth_type_t authtype, const char *user, const char *passwd)
    LOG_PPP_DISABLED_AND_DO(return ESP_ERR_NOT_SUPPORTED)

void esp_netif_ppp_set_default_netif(lwip_ppp_ctx_t* ppp_ctx)
    LOG_PPP_DISABLED_AND_DO()

lwip_ppp_ctx_t* esp_netif_new_ppp(esp_netif_t *esp_netif, const esp_netif_netstack_config_t *esp_netif_stack_config)
    LOG_PPP_DISABLED_AND_DO(return NULL)

esp_err_t esp_netif_start_ppp(lwip_ppp_ctx_t *ppp_ctx)
    LOG_PPP_DISABLED_AND_DO(return ESP_ERR_NOT_SUPPORTED)

void esp_netif_lwip_ppp_input(void *ppp_ctx, void *buffer, size_t len, void *eb)
    LOG_PPP_DISABLED_AND_DO()

esp_err_t esp_netif_stop_ppp(lwip_ppp_ctx_t *ppp_ctx)
    LOG_PPP_DISABLED_AND_DO(return ESP_ERR_NOT_SUPPORTED)

void esp_netif_destroy_ppp(lwip_ppp_ctx_t *ppp_ctx)
    LOG_PPP_DISABLED_AND_DO()

esp_err_t esp_netif_ppp_set_params(esp_netif_t *netif, const esp_netif_ppp_config_t *config)
    LOG_PPP_DISABLED_AND_DO(return ESP_ERR_NOT_SUPPORTED)

#endif /* PPPOS_SUPPORT */
