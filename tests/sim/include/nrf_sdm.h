/* Simulation stand-in for the SoftDevice manager API. */
#ifndef SIM_NRF_SDM_H
#define SIM_NRF_SDM_H
#include <stdint.h>
#include "nrf_error.h"
uint32_t sd_softdevice_is_enabled (uint8_t * p_softdevice_enabled);
#endif
