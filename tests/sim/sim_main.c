/**
 * @file sim_main.c
 * @brief RuuviTag HomeKit accessory simulator.
 *
 * Runs the firmware's HomeKit code (src/homekit + HomeKit ADK + nrf_oberon) on the
 * host with a simulated SoftDevice. A controller drives it over stdin / stdout, one
 * command per line. Responses end with a line starting with "OK"; asynchronous
 * events are reported as lines starting with "EVT".
 *
 *   ADV                        -> ADV <advertising> <interval> <addr> <adv hex> <scan rsp hex|-> <name>
 *   CONNECT <mtu>              -> OK <0|error>
 *   DISCONNECT                 -> OK 0
 *   DB                         -> DB lines, OK 0
 *   READ <handle>              -> OK <status> <hex>
 *   WRITE <handle> <hex> [long]-> OK <status>
 *   SUB <cccd> <0|1>           -> OK <status>
 *   SENSOR <temp C> <humidity> -> OK 0
 *   BATTERY <volts> <temp C>   -> OK 0
 *   TICK <ms>                  -> OK 0
 *   RESETPAIRINGS              -> (EVT RESET)
 *   STAT                       -> OK <records in Ruuvi log file>
 *   QUIT
 */

#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_homekit.h"
#include "ruuvi_driver_sensor.h"

bool sim_verbose;

static int hex_to_bytes (const char * hex, uint8_t * out, size_t max)
{
    size_t n = strlen (hex) / 2;

    if (n > max)
    {
        return -1;
    }

    for (size_t i = 0; i < n; i++)
    {
        unsigned int b;

        if (sscanf (hex + 2 * i, "%2x", &b) != 1)
        {
            return -1;
        }

        out[i] = (uint8_t) b;
    }

    return (int) n;
}

static void sensor (float t, float h)
{
    float values[2];
    rd_sensor_data_t data = { 0 };
    data.fields.datas.temperature_c = 1;
    data.fields.datas.humidity_rh = 1;
    data.data = values;
    rd_sensor_data_set (&data, RD_SENSOR_TEMP_FIELD, t);
    rd_sensor_data_set (&data, RD_SENSOR_HUMI_FIELD, h);
    app_homekit_sensor_update (&data);
}

int main (void)
{
    static char line[4096];
    static uint8_t buffer[2048];
    setvbuf (stdout, NULL, _IOLBF, 0);
    sim_verbose = getenv ("SIM_VERBOSE") != NULL;
    // Data left behind by the stock Ruuvi firmware (history log file).
    sim_fds_add_foreign_record (0xF0, 0x01);
    sim_fds_add_foreign_record (0xF0, 0xF001);

    if (app_homekit_init() != 0)
    {
        printf ("FAIL init\n");
        return 1;
    }

    sim_run_scheduler();
    sim_sd_deliver_indications();
    printf ("READY\n");

    while (fgets (line, sizeof line, stdin))
    {
        char cmd[32] = { 0 }, arg1[2048] = { 0 }, arg2[2048] = { 0 }, arg3[16] = { 0 };
        int n = sscanf (line, "%31s %2047s %2047s %15s", cmd, arg1, arg2, arg3);

        if (n < 1)
        {
            continue;
        }

        if (!strcmp (cmd, "QUIT"))
        {
            break;
        }
        else if (!strcmp (cmd, "ADV"))
        {
            sim_sd_print_advertising();
            printf ("OK 0\n");
        }
        else if (!strcmp (cmd, "CONNECT"))
        {
            printf ("OK %d\n", sim_sd_connect ( (uint16_t) atoi (arg1)));
        }
        else if (!strcmp (cmd, "DISCONNECT"))
        {
            sim_sd_disconnect();
            printf ("OK 0\n");
        }
        else if (!strcmp (cmd, "DB"))
        {
            sim_sd_print_database();
            printf ("OK 0\n");
        }
        else if (!strcmp (cmd, "READ"))
        {
            size_t len = 0;
            int e = sim_sd_read ( (uint16_t) atoi (arg1), buffer, sizeof buffer, &len);
            sim_sd_deliver_indications();
            printf ("OK %d ", e);

            for (size_t i = 0; i < len; i++)
            {
                printf ("%02x", buffer[i]);
            }

            printf ("\n");
        }
        else if (!strcmp (cmd, "WRITE"))
        {
            int len = hex_to_bytes (arg2, buffer, sizeof buffer);
            int e = len < 0 ? -1 : sim_sd_write ( (uint16_t) atoi (arg1), buffer, (size_t) len,
                                                  !strcmp (arg3, "long"));
            sim_sd_deliver_indications();
            printf ("OK %d\n", e);
        }
        else if (!strcmp (cmd, "SUB"))
        {
            int e = sim_sd_subscribe ( (uint16_t) atoi (arg1), atoi (arg2) != 0);
            sim_sd_deliver_indications();
            printf ("OK %d\n", e);
        }
        else if (!strcmp (cmd, "SENSOR"))
        {
            sensor (strtof (arg1, NULL), strtof (arg2, NULL));
            sim_run_scheduler();
            sim_sd_deliver_indications();
            printf ("OK 0\n");
        }
        else if (!strcmp (cmd, "BATTERY"))
        {
            app_homekit_battery_update (strtof (arg1, NULL), strtof (arg2, NULL));
            sim_run_scheduler();
            sim_sd_deliver_indications();
            printf ("OK 0\n");
        }
        else if (!strcmp (cmd, "TICK"))
        {
            sim_advance ( (uint64_t) atoll (arg1));
            sim_sd_deliver_indications();
            printf ("OK 0\n");
        }
        else if (!strcmp (cmd, "RESETPAIRINGS"))
        {
            app_homekit_reset_pairings();
            sim_run_scheduler();
            sim_sd_deliver_indications();
            sim_advance (1000);
            printf ("OK 0\n");
        }
        else if (!strcmp (cmd, "STAT"))
        {
            printf ("OK %u\n", sim_fds_count_file (0xF0));
        }
        else
        {
            printf ("OK -100 unknown command\n");
        }
    }

    return 0;
}
