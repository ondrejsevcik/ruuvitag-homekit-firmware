/**
 * @file sim.h
 * @brief Host simulation of the SoftDevice, FDS and Ruuvi interfaces used by src/homekit.
 */
#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern bool sim_verbose;

/* Platform (sim_platform.c). */
uint64_t sim_now (void);
void sim_advance (uint64_t ms);
void sim_run_scheduler (void);
void sim_fds_add_foreign_record (uint16_t file_id, uint16_t key);
unsigned int sim_fds_count_file (uint16_t file_id);

/* SoftDevice (sim_softdevice.c). All return 0 on success or a GATT / NRF error code. */
void sim_sd_print_advertising (void);
int sim_sd_connect (uint16_t client_mtu);
void sim_sd_disconnect (void);
void sim_sd_print_database (void);
int sim_sd_read (uint16_t handle, uint8_t * out, size_t max, size_t * len);
int sim_sd_write (uint16_t handle, const uint8_t * data, size_t len, bool force_long_write);
int sim_sd_subscribe (uint16_t cccd_handle, bool enable);
void sim_sd_deliver_indications (void);
uint16_t sim_sd_mtu (void);
bool sim_sd_is_connected (void);

#endif
