/**
 * @file sim_platform.c
 * @brief Simulated Ruuvi interfaces and Nordic FDS for host testing of src/homekit.
 *
 * - Scheduler: FIFO drained by the simulation main loop ("main context").
 * - Timers / clock: virtual time, advanced explicitly.
 * - FDS: in-memory records; completion events are delivered asynchronously,
 *   from ri_yield(), like flash operations completing in interrupt context.
 */

#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fds.h"
#include "ruuvi_driver_error.h"
#include "ruuvi_interface_log.h"
#include "ruuvi_interface_scheduler.h"
#include "ruuvi_interface_timer.h"
#include "ruuvi_interface_yield.h"

//----------------------------------------------------------------------------------------------------------------------
// Clock.

static uint64_t now_ms = 1000;

uint64_t ri_rtc_millis (void)
{
    return now_ms;
}

uint64_t sim_now (void)
{
    return now_ms;
}

//----------------------------------------------------------------------------------------------------------------------
// Scheduler.

#define SCHED_LENGTH 32
#define SCHED_EVENT_SIZE 64

typedef struct
{
    ruuvi_scheduler_event_handler_t handler;
    uint8_t data[SCHED_EVENT_SIZE];
    uint16_t size;
} sched_event_t;

static sched_event_t sched[SCHED_LENGTH];
static size_t sched_head, sched_tail;

rd_status_t ri_scheduler_event_put (const void * const p_event_data, const uint16_t event_size,
                                    const ruuvi_scheduler_event_handler_t handler)
{
    if (NULL == handler)
    {
        return RD_ERROR_NULL;
    }

    if (event_size > SCHED_EVENT_SIZE)
    {
        return RD_ERROR_INVALID_LENGTH;
    }

    size_t next = (sched_head + 1) % SCHED_LENGTH;

    if (next == sched_tail)
    {
        return RD_ERROR_NO_MEM;
    }

    sched[sched_head].handler = handler;
    sched[sched_head].size = event_size;

    if (event_size)
    {
        memcpy (sched[sched_head].data, p_event_data, event_size);
    }

    sched_head = next;
    return RD_SUCCESS;
}

void sim_run_scheduler (void)
{
    while (sched_tail != sched_head)
    {
        sched_event_t e = sched[sched_tail];
        sched_tail = (sched_tail + 1) % SCHED_LENGTH;
        e.handler (e.size ? e.data : NULL, e.size);
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Timers.

#define MAX_TIMERS 8

typedef struct
{
    bool used;
    bool running;
    ri_timer_mode_t mode;
    ruuvi_timer_timeout_handler_t handler;
    uint64_t deadline;
    uint32_t interval;
    void * context;
} sim_timer_t;

static sim_timer_t timers[MAX_TIMERS];

rd_status_t ri_timer_create (ri_timer_id_t * p_timer_id, ri_timer_mode_t mode,
                             ruuvi_timer_timeout_handler_t timeout_handler)
{
    for (size_t i = 0; i < MAX_TIMERS; i++)
    {
        if (!timers[i].used)
        {
            timers[i] = (sim_timer_t) { .used = true, .mode = mode, .handler = timeout_handler };
            *p_timer_id = &timers[i];
            return RD_SUCCESS;
        }
    }

    return RD_ERROR_RESOURCES;
}

rd_status_t ri_timer_start (ri_timer_id_t timer_id, uint32_t ms, void * const context)
{
    sim_timer_t * t = timer_id;

    if (t->running)
    {
        return RD_SUCCESS; // Like app_timer: ignored while running.
    }

    t->running = true;
    t->deadline = now_ms + ms;
    t->interval = ms;
    t->context = context;
    return RD_SUCCESS;
}

rd_status_t ri_timer_stop (ri_timer_id_t timer_id)
{
    ( (sim_timer_t *) timer_id)->running = false;
    return RD_SUCCESS;
}

void sim_advance (uint64_t ms)
{
    uint64_t target = now_ms + ms;

    for (;;)
    {
        sim_timer_t * next = NULL;

        for (size_t i = 0; i < MAX_TIMERS; i++)
        {
            if (timers[i].running && timers[i].deadline <= target
                    && (!next || timers[i].deadline < next->deadline))
            {
                next = &timers[i];
            }
        }

        if (!next)
        {
            break;
        }

        if (next->deadline > now_ms)
        {
            now_ms = next->deadline;
        }

        if (next->mode == RI_TIMER_MODE_REPEATED)
        {
            next->deadline += next->interval;
        }
        else
        {
            next->running = false;
        }

        next->handler (next->context); // "Interrupt" context.
        sim_run_scheduler();
    }

    now_ms = target;
    sim_run_scheduler();
}

//----------------------------------------------------------------------------------------------------------------------
// Misc Ruuvi interfaces.

void ri_log (const ri_log_severity_t severity, const char * const message)
{
    if (sim_verbose)
    {
        fprintf (stderr, "%s", message);
    }
}

void ri_log_hex (const ri_log_severity_t severity, const uint8_t * const bytes, size_t byte_length)
{
    if (sim_verbose)
    {
        for (size_t i = 0; i < byte_length; i++)
        {
            fprintf (stderr, "%02X", bytes[i]);
        }
    }
}

rd_status_t ri_radio_address_get (uint64_t * const address)
{
    *address = 0xFD59BB41A8CAULL;
    return RD_SUCCESS;
}

void ri_power_reset (void)
{
    printf ("EVT RESET\n");
    fflush (stdout);
    exit (0);
}

void app_led_activity_signal (const bool active)
{
    printf ("EVT LED %d\n", active ? 1 : 0);
}

//----------------------------------------------------------------------------------------------------------------------
// FDS.

#define MAX_RECORDS 128
#define MAX_RECORD_WORDS 128
#define MAX_PENDING 8

typedef struct
{
    bool valid;
    uint32_t record_id;
    fds_header_t header;
    uint32_t data[MAX_RECORD_WORDS];
} sim_record_t;

static sim_record_t records[MAX_RECORDS];
static uint32_t next_record_id = 1;
static fds_cb_t fds_handlers[4];
static fds_evt_t pending_events[MAX_PENDING];
static size_t num_pending_events;
static unsigned int dirty_records;

static void queue_fds_event (fds_evt_t evt)
{
    if (num_pending_events == MAX_PENDING)
    {
        fprintf (stderr, "sim: FDS event queue overflow\n");
        abort();
    }

    pending_events[num_pending_events++] = evt;
}

static void deliver_fds_events (void)
{
    while (num_pending_events)
    {
        fds_evt_t evt = pending_events[0];
        memmove (pending_events, pending_events + 1, (--num_pending_events) * sizeof pending_events[0]);

        for (size_t i = 0; i < 4; i++)
        {
            if (fds_handlers[i])
            {
                fds_handlers[i] (&evt);
            }
        }
    }
}

rd_status_t ri_yield (void)
{
    deliver_fds_events();
    return RD_SUCCESS;
}

ret_code_t fds_register (fds_cb_t cb)
{
    for (size_t i = 0; i < 4; i++)
    {
        if (!fds_handlers[i])
        {
            fds_handlers[i] = cb;
            return FDS_SUCCESS;
        }
    }

    return FDS_ERR_USER_LIMIT_REACHED;
}

static sim_record_t * find_by_id (uint32_t record_id)
{
    for (size_t i = 0; i < MAX_RECORDS; i++)
    {
        if (records[i].valid && records[i].record_id == record_id)
        {
            return &records[i];
        }
    }

    return NULL;
}

static ret_code_t find (uint16_t file_id, const uint16_t * key, fds_record_desc_t * desc, fds_find_token_t * token)
{
    // The token stores the index to continue from in its first word.
    uint32_t * start = (uint32_t *) token;

    for (size_t i = *start; i < MAX_RECORDS; i++)
    {
        if (records[i].valid && records[i].header.file_id == file_id && (!key || records[i].header.record_key == *key))
        {
            *start = (uint32_t) i + 1;
            memset (desc, 0, sizeof *desc);
            desc->record_id = records[i].record_id;
            return FDS_SUCCESS;
        }
    }

    *start = MAX_RECORDS;
    return FDS_ERR_NOT_FOUND;
}

ret_code_t fds_record_find (uint16_t file_id, uint16_t record_key, fds_record_desc_t * p_desc,
                            fds_find_token_t * p_token)
{
    return find (file_id, &record_key, p_desc, p_token);
}

ret_code_t fds_record_find_in_file (uint16_t file_id, fds_record_desc_t * p_desc, fds_find_token_t * p_token)
{
    return find (file_id, NULL, p_desc, p_token);
}

ret_code_t fds_record_open (fds_record_desc_t * p_desc, fds_flash_record_t * p_flash_record)
{
    sim_record_t * r = find_by_id (p_desc->record_id);

    if (!r)
    {
        return FDS_ERR_NOT_FOUND;
    }

    p_flash_record->p_header = &r->header;
    p_flash_record->p_data = r->data;
    return FDS_SUCCESS;
}

ret_code_t fds_record_close (fds_record_desc_t * p_desc)
{
    return FDS_SUCCESS;
}

static ret_code_t store (fds_record_desc_t * p_desc, fds_record_t const * p_record, fds_evt_id_t id)
{
    if ( ( (uintptr_t) p_record->data.p_data) & 3U)
    {
        return FDS_ERR_UNALIGNED_ADDR;
    }

    if (p_record->data.length_words > MAX_RECORD_WORDS)
    {
        return FDS_ERR_RECORD_TOO_LARGE;
    }

    for (size_t i = 0; i < MAX_RECORDS; i++)
    {
        if (!records[i].valid)
        {
            sim_record_t * r = &records[i];
            r->record_id = next_record_id++;
            r->header.file_id = p_record->file_id;
            r->header.record_key = p_record->key;
            r->header.length_words = p_record->data.length_words;
            // Data is copied when the write completes, like flash. Copy now; the caller must keep
            // the buffer valid anyway, which the event ordering checks.
            memcpy (r->data, p_record->data.p_data, p_record->data.length_words * 4U);
            r->valid = true;
            p_desc->record_id = r->record_id;
            queue_fds_event ( (fds_evt_t)
            {
                .id = id, .result = FDS_SUCCESS,
                .write = { .record_id = r->record_id, .file_id = p_record->file_id, .record_key = p_record->key }
            });
            return FDS_SUCCESS;
        }
    }

    return FDS_ERR_NO_SPACE_IN_FLASH;
}

ret_code_t fds_record_write (fds_record_desc_t * p_desc, fds_record_t const * p_record)
{
    return store (p_desc, p_record, FDS_EVT_WRITE);
}

ret_code_t fds_record_update (fds_record_desc_t * p_desc, fds_record_t const * p_record)
{
    sim_record_t * old = find_by_id (p_desc->record_id);

    if (!old)
    {
        return FDS_ERR_NOT_FOUND;
    }

    old->valid = false;
    dirty_records++;
    return store (p_desc, p_record, FDS_EVT_UPDATE);
}

ret_code_t fds_record_delete (fds_record_desc_t * p_desc)
{
    sim_record_t * r = find_by_id (p_desc->record_id);

    if (!r)
    {
        return FDS_ERR_NOT_FOUND;
    }

    r->valid = false;
    dirty_records++;
    queue_fds_event ( (fds_evt_t)
    {
        .id = FDS_EVT_DEL_RECORD, .result = FDS_SUCCESS,
        .del = { .record_id = r->record_id, .file_id = r->header.file_id, .record_key = r->header.record_key }
    });
    return FDS_SUCCESS;
}

ret_code_t fds_file_delete (uint16_t file_id)
{
    for (size_t i = 0; i < MAX_RECORDS; i++)
    {
        if (records[i].valid && records[i].header.file_id == file_id)
        {
            records[i].valid = false;
            dirty_records++;
        }
    }

    queue_fds_event ( (fds_evt_t)
    {
        .id = FDS_EVT_DEL_FILE, .result = FDS_SUCCESS, .del = { .file_id = file_id }
    });
    return FDS_SUCCESS;
}

ret_code_t fds_gc (void)
{
    dirty_records = 0;
    queue_fds_event ( (fds_evt_t)
    {
        .id = FDS_EVT_GC, .result = FDS_SUCCESS
    });
    return FDS_SUCCESS;
}

ret_code_t fds_stat (fds_stat_t * p_stat)
{
    memset (p_stat, 0, sizeof *p_stat);
    p_stat->dirty_records = (uint16_t) dirty_records;

    for (size_t i = 0; i < MAX_RECORDS; i++)
    {
        p_stat->valid_records += records[i].valid;
    }

    return FDS_SUCCESS;
}

void sim_fds_add_foreign_record (uint16_t file_id, uint16_t key)
{
    static uint32_t words[4] = { 1, 2, 3, 4 };
    fds_record_desc_t desc;
    fds_record_t record = { .file_id = file_id, .key = key, .data = { .p_data = words, .length_words = 4 } };
    (void) store (&desc, &record, FDS_EVT_WRITE);
    num_pending_events--; // Pre-existing data: no event.
}

unsigned int sim_fds_count_file (uint16_t file_id)
{
    unsigned int n = 0;

    for (size_t i = 0; i < MAX_RECORDS; i++)
    {
        n += records[i].valid && records[i].header.file_id == file_id;
    }

    return n;
}
