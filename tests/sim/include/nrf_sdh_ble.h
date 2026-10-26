/* Simulation: SoftDevice handler BLE observers are registered at startup. */
#ifndef SIM_NRF_SDH_BLE_H
#define SIM_NRF_SDH_BLE_H
#include "ble.h"
#include "sdk_config.h"
typedef void (*nrf_sdh_ble_evt_handler_t) (ble_evt_t const * p_ble_evt, void * p_context);
void sim_register_ble_observer (nrf_sdh_ble_evt_handler_t handler, void * context);
#define NRF_SDH_BLE_OBSERVER(_name, _prio, _handler, _context) \
    __attribute__ ((constructor)) static void _name##_register (void) \
    { sim_register_ble_observer (_handler, _context); }
#endif
