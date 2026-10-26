/* Simulation stand-in for the SoftDevice SoC API. */
#ifndef SIM_NRF_SOC_H
#define SIM_NRF_SOC_H
#include <stdint.h>
#include "nrf_error.h"
uint32_t sd_rand_application_bytes_available_get (uint8_t * p_bytes_available);
uint32_t sd_rand_application_vector_get (uint8_t * p_buff, uint8_t length);
#endif
