/**
 * @file app_comms_homekit.c
 * @brief Communication setup of the HomeKit variant.
 *
 * Bluetooth belongs to HomeKit (src/homekit). NFC is kept so that the Ruuvi app
 * can still read the device info and current readings by tapping the tag.
 * Configuration over GATT / NFC commands is not available.
 */
#include "app_config.h"
#include "app_comms.h"

#if APP_HOMEKIT_ENABLED

#include "app_homekit.h"
#include "app_sensor.h"
#include "ruuvi_boards.h"
#include "ruuvi_interface_communication.h"
#include "ruuvi_interface_communication_ble_advertising.h"
#include "ruuvi_interface_communication_radio.h"
#include "ruuvi_interface_rtc.h"
#include "ruuvi_interface_yield.h"
#include "ruuvi_task_communication.h"
#include "ruuvi_task_nfc.h"
#include <stdio.h>
#include <string.h>

#define BLOCKING_COMM_TIMEOUT_MS (4000U)

static rd_status_t dis_init (ri_comm_dis_init_t * const p_dis)
{
    rd_status_t err_code = RD_SUCCESS;
    size_t name_idx = 0;
    memset (p_dis, 0, sizeof (ri_comm_dis_init_t));
    err_code |= rt_com_get_mac_str (p_dis->deviceaddr, sizeof (p_dis->deviceaddr));
    err_code |= rt_com_get_id_str (p_dis->deviceid, sizeof (p_dis->deviceid));
    name_idx =  snprintf (p_dis->fw_version, sizeof (p_dis->fw_version), APP_FW_NAME);
    name_idx += snprintf (p_dis->fw_version + name_idx,
                          sizeof (p_dis->fw_version) - name_idx,
                          APP_FW_VERSION);
    snprintf (p_dis->fw_version + name_idx,
              sizeof (p_dis->fw_version) - name_idx,
              APP_FW_VARIANT);
    snprintf (p_dis->hw_version, sizeof (p_dis->hw_version), "Check PCB");
    snprintf (p_dis->manufacturer, sizeof (p_dis->manufacturer), RB_MANUFACTURER_STRING);
    snprintf (p_dis->model, sizeof (p_dis->model), RB_MODEL_STRING);
    return err_code;
}

rd_status_t app_comms_init (const bool secure)
{
    (void) secure;
    rd_status_t err_code = RD_SUCCESS;
    err_code |= ri_radio_init (APP_MODULATION);

    if (RD_SUCCESS == err_code)
    {
#if APP_NFC_ENABLED
        ri_comm_dis_init_t dis = {0};
        err_code |= dis_init (&dis);
        err_code |= rt_nfc_init (&dis);
#endif
        // Battery voltage is sampled right after radio activity, under load.
        ri_radio_activity_callback_set (&app_sensor_vdd_measure_isr);
        err_code |= app_homekit_init();
    }

    return err_code;
}

/**
 * @brief TX power reported in the Ruuvi data formats (shown over NFC).
 *
 * Replaces the Ruuvi advertising module, which is not built in the HomeKit variant.
 */
rd_status_t ri_adv_tx_power_get (int8_t * dbm)
{
    if (NULL == dbm)
    {
        return RD_ERROR_NULL;
    }

    *dbm = APP_HOMEKIT_TX_POWER_DBM;
    return RD_SUCCESS;
}

uint8_t app_comms_bleadv_send_count_get (void)
{
    return APP_COMM_ADV_DISABLE;
}

void app_comms_bleadv_send_count_set (const uint8_t count)
{
    (void) count;
}

rd_status_t app_comms_configure_next_enable (void)
{
    return RD_SUCCESS;
}

rd_status_t app_comms_configure_next_disable (void)
{
    return RD_SUCCESS;
}

rd_status_t app_comms_ble_init (const bool secure)
{
    (void) secure;
    return RD_SUCCESS;
}

rd_status_t app_comms_ble_adv_init (void)
{
    return RD_SUCCESS;
}

rd_status_t app_comms_ble_uninit (void)
{
    return RD_SUCCESS;
}

rd_status_t app_comms_blocking_send (const ri_comm_xfer_fp_t reply_fp,
                                     ri_comm_message_t * const msg)
{
    rd_status_t err_code = RD_SUCCESS;
    const uint64_t start = ri_rtc_millis();

    do
    {
        err_code = (ri_rtc_millis() > (start + BLOCKING_COMM_TIMEOUT_MS)) ?
                   RD_ERROR_TIMEOUT : reply_fp (msg);

        if (RD_ERROR_NO_MEM == err_code)
        {
            ri_yield();
        }
    } while ( (RD_SUCCESS != err_code) && ! (RD_ERROR_TIMEOUT & err_code));

    return err_code;
}

#endif // APP_HOMEKIT_ENABLED
