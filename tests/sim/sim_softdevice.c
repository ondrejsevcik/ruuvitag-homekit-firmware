/**
 * @file sim_softdevice.c
 * @brief Simulated S132 SoftDevice (GAP / GATTS subset) for host testing of src/homekit.
 *
 * Models the behaviour documented in the S132 v6.1.1 API and message sequence charts:
 * - Authorized reads: one BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST per Read / Read Blob request.
 * - Authorized writes: Write Request, or Prepare Write + Execute Write using the
 *   application supplied queued write buffer (BLE_EVT_USER_MEM_REQUEST).
 * - Indications: one outstanding at a time, confirmed with BLE_GATTS_EVT_HVC.
 * - Advertising: parameters only change while stopped, data updates need new buffers.
 * The peer (central) side is driven by the simulation main loop.
 */

#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble.h"
#include "nrf_sdh_ble.h"
#include "nrf_nvic.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"

#define CONN_HANDLE 0x0001
#define MAX_ATTRS 160
#define MAX_BASES 8
#define MAX_VALUE 512

typedef enum { ATT_SERVICE, ATT_DECL, ATT_VALUE, ATT_CCCD, ATT_DESC } att_kind_t;

typedef struct
{
    att_kind_t kind;
    uint16_t handle;
    ble_uuid_t uuid;
    // Characteristic declaration.
    ble_gatt_char_props_t props;
    uint16_t value_handle;
    uint16_t cccd_handle;
    // Value / descriptor.
    bool rd_auth, wr_auth, user_loc, vlen;
    bool readable, writable;
    uint8_t * value;        //!< User memory or malloc'd stack memory.
    uint16_t len, max_len;
} att_t;

static nrf_sdh_ble_evt_handler_t observers[4];
static void * observer_contexts[4];
static size_t num_observers;

static att_t attrs[MAX_ATTRS];
static size_t num_attrs;
static uint16_t next_handle = 0x0010; // After the GAP / GATT services.
static ble_uuid128_t bases[MAX_BASES];
static size_t num_bases;

static bool connected;
static uint16_t mtu = 23;
static bool advertising;
static bool adv_configured;
static ble_gap_adv_data_t adv_data;
static ble_gap_adv_params_t adv_params;
static ble_gap_addr_t address;
static char device_name[65];

static struct
{
    bool pending, replied;
    uint8_t type;
    uint16_t handle;
    ble_gatts_rw_authorize_reply_params_t reply;
    uint8_t reply_data[MAX_VALUE];
} auth;

static ble_user_mem_block_t user_mem;
static bool user_mem_replied;
static uint16_t indication_pending_handle;
static bool disconnect_requested;

void sim_register_ble_observer (nrf_sdh_ble_evt_handler_t handler, void * context)
{
    observers[num_observers] = handler;
    observer_contexts[num_observers++] = context;
}

static void dispatch (ble_evt_t * evt)
{
    for (size_t i = 0; i < num_observers; i++)
    {
        observers[i] (evt, observer_contexts[i]);
    }
}

static ble_evt_t * new_event (uint16_t id)
{
    static uint32_t buffer[(sizeof (ble_evt_t) + MAX_VALUE + 64) / 4];
    memset (buffer, 0, sizeof buffer);
    ble_evt_t * evt = (ble_evt_t *) buffer;
    evt->header.evt_id = id;
    evt->header.evt_len = sizeof buffer;
    return evt;
}

static att_t * find_attr (uint16_t handle)
{
    for (size_t i = 0; i < num_attrs; i++)
    {
        if (attrs[i].handle == handle)
        {
            return &attrs[i];
        }
    }

    return NULL;
}

static att_t * add_attr (att_kind_t kind)
{
    if (num_attrs == MAX_ATTRS)
    {
        return NULL;
    }

    att_t * a = &attrs[num_attrs++];
    memset (a, 0, sizeof *a);
    a->kind = kind;
    a->handle = next_handle++;
    return a;
}

static void uuid_to_string (const ble_uuid_t * u, char * out)
{
    uint8_t b[16];

    if (u->type == BLE_UUID_TYPE_BLE)
    {
        sprintf (out, "%04x", u->uuid);
        return;
    }

    memcpy (b, bases[u->type - BLE_UUID_TYPE_VENDOR_BEGIN].uuid128, 16);
    b[12] = (uint8_t) u->uuid;
    b[13] = (uint8_t) (u->uuid >> 8);
    sprintf (out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[15], b[14], b[13], b[12], b[11], b[10], b[9], b[8], b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]);
}

static void set_value (att_t * a, const ble_gatts_attr_t * attr)
{
    a->rd_auth = attr->p_attr_md->rd_auth;
    a->wr_auth = attr->p_attr_md->wr_auth;
    a->vlen = attr->p_attr_md->vlen;
    a->readable = attr->p_attr_md->read_perm.sm != 0;
    a->writable = attr->p_attr_md->write_perm.sm != 0;
    a->max_len = attr->max_len;
    a->len = attr->init_len;
    a->user_loc = attr->p_attr_md->vloc == BLE_GATTS_VLOC_USER;

    if (a->user_loc)
    {
        a->value = attr->p_value;
    }
    else
    {
        a->value = calloc (1, attr->max_len ? attr->max_len : 1);

        if (attr->p_value && attr->init_len)
        {
            memcpy (a->value, attr->p_value, attr->init_len);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// SoftDevice API.

uint32_t sd_ble_uuid_vs_add (ble_uuid128_t const * p_vs_uuid, uint8_t * p_uuid_type)
{
    for (size_t i = 0; i < num_bases; i++)
    {
        ble_uuid128_t a = bases[i], b = *p_vs_uuid;
        a.uuid128[12] = a.uuid128[13] = b.uuid128[12] = b.uuid128[13] = 0;

        if (!memcmp (&a, &b, sizeof a))
        {
            *p_uuid_type = (uint8_t) (BLE_UUID_TYPE_VENDOR_BEGIN + i);
            return NRF_SUCCESS;
        }
    }

    if (num_bases >= 6) // NRF_SDH_BLE_VS_UUID_COUNT of the HomeKit variant.
    {
        return NRF_ERROR_NO_MEM;
    }

    bases[num_bases] = *p_vs_uuid;
    *p_uuid_type = (uint8_t) (BLE_UUID_TYPE_VENDOR_BEGIN + num_bases++);
    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_service_add (uint8_t type, ble_uuid_t const * p_uuid, uint16_t * p_handle)
{
    att_t * a = add_attr (ATT_SERVICE);

    if (!a)
    {
        return NRF_ERROR_NO_MEM;
    }

    a->uuid = *p_uuid;
    *p_handle = a->handle;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_characteristic_add (uint16_t service_handle, ble_gatts_char_md_t const * p_char_md,
        ble_gatts_attr_t const * p_attr_char_value, ble_gatts_char_handles_t * p_handles)
{
    if (p_attr_char_value->max_len > (p_attr_char_value->p_attr_md->vlen ? BLE_GATTS_VAR_ATTR_LEN_MAX :
                                      BLE_GATTS_FIX_ATTR_LEN_MAX))
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    if (p_attr_char_value->p_attr_md->vloc == BLE_GATTS_VLOC_USER && !p_attr_char_value->p_value)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    att_t * decl = add_attr (ATT_DECL);
    att_t * value = add_attr (ATT_VALUE);

    if (!decl || !value)
    {
        return NRF_ERROR_NO_MEM;
    }

    decl->uuid = *p_attr_char_value->p_uuid;
    decl->props = p_char_md->char_props;
    value->uuid = *p_attr_char_value->p_uuid;
    value->props = p_char_md->char_props;
    set_value (value, p_attr_char_value);
    decl->value_handle = value->handle;
    p_handles->value_handle = value->handle;
    p_handles->cccd_handle = BLE_GATT_HANDLE_INVALID;
    p_handles->user_desc_handle = BLE_GATT_HANDLE_INVALID;
    p_handles->sccd_handle = BLE_GATT_HANDLE_INVALID;

    if (p_char_md->char_props.notify || p_char_md->char_props.indicate)
    {
        if (!p_char_md->p_cccd_md)
        {
            return NRF_ERROR_INVALID_PARAM;
        }

        att_t * cccd = add_attr (ATT_CCCD);
        cccd->uuid = (ble_uuid_t) { .uuid = BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG, .type = BLE_UUID_TYPE_BLE };
        cccd->value = calloc (1, 2);
        cccd->len = cccd->max_len = 2;
        cccd->readable = cccd->writable = true;
        decl->cccd_handle = cccd->handle;
        value->cccd_handle = cccd->handle;
        p_handles->cccd_handle = cccd->handle;
    }

    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_descriptor_add (uint16_t char_handle, ble_gatts_attr_t const * p_attr, uint16_t * p_handle)
{
    att_t * a = add_attr (ATT_DESC);

    if (!a)
    {
        return NRF_ERROR_NO_MEM;
    }

    a->uuid = *p_attr->p_uuid;
    set_value (a, p_attr);
    *p_handle = a->handle;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_rw_authorize_reply (uint16_t conn_handle,
        ble_gatts_rw_authorize_reply_params_t const * p_reply)
{
    if (!connected || conn_handle != CONN_HANDLE)
    {
        return BLE_ERROR_INVALID_CONN_HANDLE;
    }

    if (!auth.pending || auth.replied)
    {
        fprintf (stderr, "sim: authorize reply without pending request\n");
        return NRF_ERROR_INVALID_STATE;
    }

    if (p_reply->type != auth.type)
    {
        fprintf (stderr, "sim: authorize reply type mismatch\n");
        return NRF_ERROR_INVALID_PARAM;
    }

    const ble_gatts_authorize_params_t * p = auth.type == BLE_GATTS_AUTHORIZE_TYPE_READ ?
            &p_reply->params.read : &p_reply->params.write;

    if (p->update && p->len && !p->p_data)
    {
        return NRF_ERROR_INVALID_ADDR;
    }

    auth.reply = *p_reply;

    if (p->update && p->len)
    {
        memcpy (auth.reply_data, p->p_data, p->len);
    }

    auth.replied = true;
    return NRF_SUCCESS;
}

uint32_t sd_ble_user_mem_reply (uint16_t conn_handle, ble_user_mem_block_t const * p_block)
{
    if (p_block)
    {
        user_mem = *p_block;
    }
    else
    {
        memset (&user_mem, 0, sizeof user_mem);
    }

    user_mem_replied = true;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_hvx (uint16_t conn_handle, ble_gatts_hvx_params_t const * p_hvx_params)
{
    if (!connected || conn_handle != CONN_HANDLE)
    {
        return BLE_ERROR_INVALID_CONN_HANDLE;
    }

    att_t * a = find_attr (p_hvx_params->handle);

    if (!a || a->kind != ATT_VALUE || !a->cccd_handle)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    att_t * cccd = find_attr (a->cccd_handle);

    if (p_hvx_params->type != BLE_GATT_HVX_INDICATION || ! (cccd->value[0] & BLE_GATT_HVX_INDICATION))
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (indication_pending_handle)
    {
        return NRF_ERROR_BUSY;
    }

    if (p_hvx_params->p_data && *p_hvx_params->p_len)
    {
        memcpy (a->value, p_hvx_params->p_data, *p_hvx_params->p_len);
        a->len = *p_hvx_params->p_len;
    }

    indication_pending_handle = a->handle;
    printf ("EVT IND %u\n", a->handle);
    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_sys_attr_set (uint16_t conn_handle, uint8_t const * p_sys_attr_data, uint16_t len,
                                    uint32_t flags)
{
    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_exchange_mtu_reply (uint16_t conn_handle, uint16_t server_rx_mtu)
{
    if (server_rx_mtu > NRF_SDH_BLE_GATT_MAX_MTU_SIZE)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    mtu = server_rx_mtu < mtu ? server_rx_mtu : mtu;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_sec_params_reply (uint16_t conn_handle, uint8_t sec_status,
                                      ble_gap_sec_params_t const * p_sec_params, ble_gap_sec_keyset_t const * p_sec_keyset)
{
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_phy_update (uint16_t conn_handle, ble_gap_phys_t const * p_gap_phys)
{
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_data_length_update (uint16_t conn_handle, ble_gap_data_length_params_t const * p_dl_params,
                                        ble_gap_data_length_limitation_t * p_dl_limitation)
{
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_disconnect (uint16_t conn_handle, uint8_t hci_status_code)
{
    if (!connected || conn_handle != CONN_HANDLE)
    {
        return BLE_ERROR_INVALID_CONN_HANDLE;
    }

    disconnect_requested = true; // Event follows asynchronously.
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_tx_power_set (uint8_t role, uint16_t handle, int8_t tx_power)
{
    static const int8_t valid[] = { -40, -20, -16, -12, -8, -4, 0, 3, 4 };

    for (size_t i = 0; i < sizeof valid; i++)
    {
        if (valid[i] == tx_power)
        {
            return NRF_SUCCESS;
        }
    }

    return NRF_ERROR_INVALID_PARAM;
}

uint32_t sd_ble_gap_addr_set (ble_gap_addr_t const * p_addr)
{
    if (advertising)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (p_addr->addr_type == BLE_GAP_ADDR_TYPE_RANDOM_STATIC && (p_addr->addr[5] & 0xC0) != 0xC0)
    {
        return BLE_ERROR_GAP_INVALID_BLE_ADDR;
    }

    address = *p_addr;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_device_name_set (ble_gap_conn_sec_mode_t const * p_write_perm, uint8_t const * p_dev_name,
                                     uint16_t len)
{
    if (len >= sizeof device_name)
    {
        return NRF_ERROR_DATA_SIZE;
    }

    memcpy (device_name, p_dev_name, len);
    device_name[len] = 0;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_adv_set_configure (uint8_t * p_adv_handle, ble_gap_adv_data_t const * p_adv_data,
                                       ble_gap_adv_params_t const * p_adv_params)
{
    if (*p_adv_handle == BLE_GAP_ADV_SET_HANDLE_NOT_SET)
    {
        if (!p_adv_params)
        {
            return NRF_ERROR_INVALID_PARAM;
        }

        *p_adv_handle = 0;
    }
    else if (*p_adv_handle != 0)
    {
        return BLE_ERROR_INVALID_ADV_HANDLE;
    }

    if (advertising)
    {
        if (p_adv_params)
        {
            return NRF_ERROR_INVALID_STATE; // Parameters cannot change while advertising.
        }

        if (p_adv_data && (p_adv_data->adv_data.p_data == adv_data.adv_data.p_data
                           || (p_adv_data->scan_rsp_data.p_data
                               && p_adv_data->scan_rsp_data.p_data == adv_data.scan_rsp_data.p_data)))
        {
            return NRF_ERROR_INVALID_STATE; // New buffers required while advertising.
        }
    }

    if (p_adv_data)
    {
        if (p_adv_data->adv_data.len > 31 || p_adv_data->scan_rsp_data.len > 31)
        {
            return NRF_ERROR_INVALID_DATA;
        }

        adv_data = *p_adv_data;
    }

    if (p_adv_params)
    {
        if (p_adv_params->interval < BLE_GAP_ADV_INTERVAL_MIN || p_adv_params->interval > BLE_GAP_ADV_INTERVAL_MAX)
        {
            return NRF_ERROR_INVALID_PARAM;
        }

        adv_params = *p_adv_params;
    }

    adv_configured = true;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_adv_start (uint8_t adv_handle, uint8_t conn_cfg_tag)
{
    if (!adv_configured || adv_handle != 0)
    {
        return BLE_ERROR_INVALID_ADV_HANDLE;
    }

    if (advertising)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (connected)
    {
        return NRF_ERROR_CONN_COUNT;
    }

    advertising = true;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_adv_stop (uint8_t adv_handle)
{
    if (!advertising)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    advertising = false;
    return NRF_SUCCESS;
}

uint32_t sd_nvic_SystemReset (void)
{
    printf ("EVT RESET\n");
    fflush (stdout);
    exit (0);
}

void NVIC_SystemReset (void)
{
    sd_nvic_SystemReset();
}

uint32_t sd_softdevice_is_enabled (uint8_t * p_softdevice_enabled)
{
    *p_softdevice_enabled = 1;
    return NRF_SUCCESS;
}

uint32_t sd_rand_application_bytes_available_get (uint8_t * p_bytes_available)
{
    *p_bytes_available = 32;
    return NRF_SUCCESS;
}

uint32_t sd_rand_application_vector_get (uint8_t * p_buff, uint8_t length)
{
    static FILE * f;

    if (!f)
    {
        f = fopen ("/dev/urandom", "rb");
    }

    if (length > 32 || fread (p_buff, 1, length, f) != length)
    {
        return NRF_ERROR_NO_MEM;
    }

    return NRF_SUCCESS;
}

//----------------------------------------------------------------------------------------------------------------------
// Central side, driven by the simulation.

uint16_t sim_sd_mtu (void)
{
    return mtu;
}

bool sim_sd_is_connected (void)
{
    return connected;
}

static void print_hex (const uint8_t * p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        printf ("%02x", p[i]);
    }
}

void sim_sd_print_advertising (void)
{
    printf ("ADV %d %u %02x%02x%02x%02x%02x%02x ", advertising ? 1 : 0, adv_params.interval,
            address.addr[5], address.addr[4], address.addr[3], address.addr[2], address.addr[1], address.addr[0]);
    print_hex (adv_data.adv_data.p_data, adv_data.adv_data.len);
    printf (" ");

    if (adv_data.scan_rsp_data.len)
    {
        print_hex (adv_data.scan_rsp_data.p_data, adv_data.scan_rsp_data.len);
    }
    else
    {
        printf ("-");
    }

    printf (" %s\n", device_name);
}

int sim_sd_connect (uint16_t client_mtu)
{
    if (!advertising)
    {
        return -1;
    }

    advertising = false;
    connected = true;
    mtu = 23;
    ble_evt_t * evt = new_event (BLE_GAP_EVT_CONNECTED);
    evt->evt.gap_evt.conn_handle = CONN_HANDLE;
    evt->evt.gap_evt.params.connected.role = BLE_GAP_ROLE_PERIPH;
    evt->evt.gap_evt.params.connected.adv_handle = 0;
    dispatch (evt);
    sim_run_scheduler();
    // The central starts with an MTU exchange, as iOS does.
    mtu = client_mtu;
    evt = new_event (BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST);
    evt->evt.gatts_evt.conn_handle = CONN_HANDLE;
    evt->evt.gatts_evt.params.exchange_mtu_request.client_rx_mtu = client_mtu;
    dispatch (evt);
    sim_run_scheduler();
    return 0;
}

static void disconnected (uint8_t reason)
{
    connected = false;
    indication_pending_handle = 0;

    for (size_t i = 0; i < num_attrs; i++)
    {
        if (attrs[i].kind == ATT_CCCD)
        {
            attrs[i].value[0] = attrs[i].value[1] = 0;
        }
    }

    ble_evt_t * evt = new_event (BLE_GAP_EVT_DISCONNECTED);
    evt->evt.gap_evt.conn_handle = CONN_HANDLE;
    evt->evt.gap_evt.params.disconnected.reason = reason;
    dispatch (evt);
    sim_run_scheduler();
}

void sim_sd_disconnect (void)
{
    if (connected)
    {
        disconnected (BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    }
}

void sim_sd_deliver_indications (void)
{
    // Asynchronous SoftDevice events: indication confirmations and requested disconnects.
    for (int guard = 0; guard < 16; guard++)
    {
        if (indication_pending_handle && connected)
        {
            uint16_t handle = indication_pending_handle;
            indication_pending_handle = 0;
            ble_evt_t * evt = new_event (BLE_GATTS_EVT_HVC);
            evt->evt.gatts_evt.conn_handle = CONN_HANDLE;
            evt->evt.gatts_evt.params.hvc.handle = handle;
            dispatch (evt);
            sim_run_scheduler();
        }
        else if (disconnect_requested)
        {
            disconnect_requested = false;

            if (connected)
            {
                printf ("EVT DISCONNECTED\n");
                disconnected (BLE_HCI_LOCAL_HOST_TERMINATED_CONNECTION);
            }
        }
        else
        {
            return;
        }
    }
}

void sim_sd_print_database (void)
{
    char uuid[40];

    for (size_t i = 0; i < num_attrs; i++)
    {
        att_t * a = &attrs[i];
        uuid_to_string (&a->uuid, uuid);

        switch (a->kind)
        {
            case ATT_SERVICE:
                printf ("DB S %u %s\n", a->handle, uuid);
                break;

            case ATT_DECL:
                printf ("DB C %u %u %s %u%u%u%u%u\n", a->handle, a->value_handle, uuid, a->props.read,
                        a->props.write_wo_resp, a->props.write, a->props.notify, a->props.indicate);
                break;

            case ATT_CCCD:
            case ATT_DESC:
                printf ("DB D %u %s\n", a->handle, uuid);
                break;

            default:
                break;
        }
    }
}

/** Sends an authorize request and checks that the application replied in main context. */
static int authorize (ble_evt_t * evt, uint8_t type, uint16_t handle)
{
    auth.pending = true;
    auth.replied = false;
    auth.type = type;
    auth.handle = handle;
    dispatch (evt);

    if (auth.replied)
    {
        fprintf (stderr, "sim: authorize reply from interrupt context (allowed, but unexpected)\n");
    }

    sim_run_scheduler();
    auth.pending = false;

    if (!auth.replied)
    {
        fprintf (stderr, "sim: no authorize reply for handle %u\n", handle);
        return -2;
    }

    return 0;
}

int sim_sd_read (uint16_t handle, uint8_t * out, size_t max, size_t * len)
{
    att_t * a = find_attr (handle);
    *len = 0;

    if (!connected)
    {
        return -1;
    }

    if (!a || (a->kind != ATT_VALUE && a->kind != ATT_DESC && a->kind != ATT_CCCD) || !a->readable)
    {
        return BLE_GATT_STATUS_ATTERR_READ_NOT_PERMITTED;
    }

    // Read Request, then Read Blob Requests while responses are full (ATT long read).
    uint16_t offset = 0;

    for (;;)
    {
        if (a->rd_auth)
        {
            ble_evt_t * evt = new_event (BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST);
            evt->evt.gatts_evt.conn_handle = CONN_HANDLE;
            evt->evt.gatts_evt.params.authorize_request.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
            evt->evt.gatts_evt.params.authorize_request.request.read.handle = handle;
            evt->evt.gatts_evt.params.authorize_request.request.read.uuid = a->uuid;
            evt->evt.gatts_evt.params.authorize_request.request.read.offset = offset;
            int e = authorize (evt, BLE_GATTS_AUTHORIZE_TYPE_READ, handle);

            if (e)
            {
                return e;
            }

            if (auth.reply.params.read.gatt_status != BLE_GATT_STATUS_SUCCESS)
            {
                return auth.reply.params.read.gatt_status;
            }

            if (auth.reply.params.read.update)
            {
                uint16_t o = auth.reply.params.read.offset, l = auth.reply.params.read.len;

                if (o + l > a->max_len)
                {
                    fprintf (stderr, "sim: read reply exceeds max_len\n");
                    return -3;
                }

                memcpy (a->value + o, auth.reply_data, l);
                a->len = o + l;
            }
        }

        if (offset > a->len)
        {
            return BLE_GATT_STATUS_ATTERR_INVALID_OFFSET;
        }

        size_t n = a->len - offset;

        if (n > (size_t) (mtu - 1))
        {
            n = mtu - 1;
        }

        if (*len + n > max)
        {
            return -4;
        }

        memcpy (out + *len, a->value + offset, n);
        *len += n;
        offset += n;

        if (n < (size_t) (mtu - 1))
        {
            return 0;
        }
    }
}

static int write_request (att_t * a, const uint8_t * data, size_t len)
{
    if (a->kind == ATT_CCCD)
    {
        if (len != 2)
        {
            return BLE_GATT_STATUS_ATTERR_INVALID_ATT_VAL_LENGTH;
        }

        memcpy (a->value, data, 2);
        // The SoftDevice stores CCCD values itself and reports the write afterwards.
        ble_evt_t * evt = new_event (BLE_GATTS_EVT_WRITE);
        evt->evt.gatts_evt.conn_handle = CONN_HANDLE;
        ble_gatts_evt_write_t * w = &evt->evt.gatts_evt.params.write;
        w->handle = a->handle;
        w->uuid = a->uuid;
        w->op = BLE_GATTS_OP_WRITE_REQ;
        w->len = 2;
        memcpy (w->data, data, 2);
        dispatch (evt);
        sim_run_scheduler();
        return 0;
    }

    if (a->wr_auth)
    {
        ble_evt_t * evt = new_event (BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST);
        evt->evt.gatts_evt.conn_handle = CONN_HANDLE;
        evt->evt.gatts_evt.params.authorize_request.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
        ble_gatts_evt_write_t * w = &evt->evt.gatts_evt.params.authorize_request.request.write;
        w->handle = a->handle;
        w->uuid = a->uuid;
        w->op = BLE_GATTS_OP_WRITE_REQ;
        w->auth_required = 1;
        w->offset = 0;
        w->len = (uint16_t) len;
        memcpy (w->data, data, len);
        int e = authorize (evt, BLE_GATTS_AUTHORIZE_TYPE_WRITE, a->handle);

        if (e)
        {
            return e;
        }

        if (auth.reply.params.write.gatt_status != BLE_GATT_STATUS_SUCCESS)
        {
            return auth.reply.params.write.gatt_status;
        }

        if (!auth.reply.params.write.update)
        {
            fprintf (stderr, "sim: write authorize reply without update flag\n");
            return -5;
        }

        memcpy (a->value, data, len);
        a->len = (uint16_t) len;
        return 0;
    }

    memcpy (a->value, data, len);
    a->len = (uint16_t) len;
    return 0;
}

static int long_write (att_t * a, const uint8_t * data, size_t len)
{
    ble_evt_t * evt = new_event (BLE_EVT_USER_MEM_REQUEST);
    evt->evt.common_evt.conn_handle = CONN_HANDLE;
    evt->evt.common_evt.params.user_mem_request.type = BLE_USER_MEM_TYPE_GATTS_QUEUED_WRITES;
    user_mem_replied = false;
    dispatch (evt);

    if (!user_mem_replied || !user_mem.p_mem)
    {
        fprintf (stderr, "sim: no queued write memory\n");
        return BLE_GATT_STATUS_ATTERR_PREPARE_QUEUE_FULL;
    }

    // Prepare Write Requests.
    size_t chunk = mtu - 5, q = 0;

    for (size_t offset = 0; offset < len; offset += chunk)
    {
        size_t n = len - offset < chunk ? len - offset : chunk;

        if (q + 6 + n + 2 > user_mem.len)
        {
            return BLE_GATT_STATUS_ATTERR_PREPARE_QUEUE_FULL;
        }

        uint8_t * m = user_mem.p_mem + q;
        m[0] = (uint8_t) a->handle;
        m[1] = (uint8_t) (a->handle >> 8);
        m[2] = (uint8_t) offset;
        m[3] = (uint8_t) (offset >> 8);
        m[4] = (uint8_t) n;
        m[5] = (uint8_t) (n >> 8);
        memcpy (m + 6, data + offset, n);
        q += 6 + n;

        if (a->wr_auth)
        {
            evt = new_event (BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST);
            evt->evt.gatts_evt.conn_handle = CONN_HANDLE;
            evt->evt.gatts_evt.params.authorize_request.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
            ble_gatts_evt_write_t * w = &evt->evt.gatts_evt.params.authorize_request.request.write;
            w->handle = a->handle;
            w->op = BLE_GATTS_OP_PREP_WRITE_REQ;
            w->offset = (uint16_t) offset;
            w->len = (uint16_t) n;
            memcpy (w->data, data + offset, n);
            int e = authorize (evt, BLE_GATTS_AUTHORIZE_TYPE_WRITE, a->handle);

            if (e)
            {
                return e;
            }

            if (auth.reply.params.write.gatt_status != BLE_GATT_STATUS_SUCCESS)
            {
                return auth.reply.params.write.gatt_status;
            }
        }
    }

    user_mem.p_mem[q] = 0; // Terminating handle (BLE_GATT_HANDLE_INVALID).
    user_mem.p_mem[q + 1] = 0;
    // Execute Write Request.
    evt = new_event (BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST);
    evt->evt.gatts_evt.conn_handle = CONN_HANDLE;
    evt->evt.gatts_evt.params.authorize_request.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
    evt->evt.gatts_evt.params.authorize_request.request.write.op = BLE_GATTS_OP_EXEC_WRITE_REQ_NOW;
    int e = authorize (evt, BLE_GATTS_AUTHORIZE_TYPE_WRITE, BLE_GATT_HANDLE_INVALID);
    evt = new_event (BLE_EVT_USER_MEM_RELEASE);
    evt->evt.common_evt.conn_handle = CONN_HANDLE;
    evt->evt.common_evt.params.user_mem_release.type = BLE_USER_MEM_TYPE_GATTS_QUEUED_WRITES;
    evt->evt.common_evt.params.user_mem_release.mem_block = user_mem;
    dispatch (evt);

    if (e)
    {
        return e;
    }

    return auth.reply.params.write.gatt_status;
}

int sim_sd_write (uint16_t handle, const uint8_t * data, size_t len, bool force_long_write)
{
    att_t * a = find_attr (handle);

    if (!connected)
    {
        return -1;
    }

    if (!a || !a->writable)
    {
        return BLE_GATT_STATUS_ATTERR_WRITE_NOT_PERMITTED;
    }

    if (len > a->max_len)
    {
        return BLE_GATT_STATUS_ATTERR_INVALID_ATT_VAL_LENGTH;
    }

    if (len <= (size_t) (mtu - 3) && !force_long_write)
    {
        return write_request (a, data, len);
    }

    return long_write (a, data, len);
}

int sim_sd_subscribe (uint16_t cccd_handle, bool enable)
{
    uint8_t v[2] = { enable ? BLE_GATT_HVX_INDICATION : 0, 0 };
    return sim_sd_write (cccd_handle, v, sizeof v, false);
}
