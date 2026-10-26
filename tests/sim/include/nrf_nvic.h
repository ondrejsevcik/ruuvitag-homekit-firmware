/* Simulation stand-in for the SoftDevice NVIC API. */
#ifndef SIM_NRF_NVIC_H
#define SIM_NRF_NVIC_H
#include <stdint.h>
#include "nrf_error.h"
uint32_t sd_nvic_SystemReset (void);
void NVIC_SystemReset (void);
#endif
