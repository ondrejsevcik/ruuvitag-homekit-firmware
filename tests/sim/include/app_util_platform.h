/* Simulation: single threaded, interrupts are delivered synchronously. */
#ifndef SIM_APP_UTIL_PLATFORM_H
#define SIM_APP_UTIL_PLATFORM_H
#define CRITICAL_REGION_ENTER() {
#define CRITICAL_REGION_EXIT()  }
#ifndef ANON_UNIONS_ENABLE
#define ANON_UNIONS_ENABLE  struct sim_semicolon_swallower
#define ANON_UNIONS_DISABLE struct sim_semicolon_swallower
#endif
#endif
