/* Cadence model.
 *
 * A CSC sensor reports, per crank revolution, (cumulative revolutions, event
 * time of that revolution in 1/1024 s).  Clients derive cadence from the
 * difference of two such reports, so the reported event times have to be spaced
 * like real revolutions - not like our tick or notification rate.
 *
 * The model integrates crank angle, and when a revolution completes it
 * interpolates the instant it completed to microsecond resolution.  That keeps
 * the derived cadence exact and jitter-free instead of quantised to the tick.
 */
#include "csc_sim.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "csc-sim";

/* One revolution is 60e6 micro-revolutions, so at N rpm the crank advances
 * exactly N micro-revolutions per microsecond: no scale factor in the integrator. */
#define REV_UREV            60000000ULL
#define TICK_US             50000                  /* model tick, 20 Hz */

/* Cadence slew rates, rpm per second. */
#define RAMP_CMD_RPM_S      90.0f                  /* `rpm <n>` */
#define RAMP_STOP_RPM_S     70.0f
#define RAMP_SPINUP_RPM_S   11.0f                  /* from standstill, like a rider */
#define RAMP_COAST_RPM_S    35.0f

/* Scripted ride profile. */
#define RIDE_CRUISE_DEFAULT 85
#define RIDE_HOLD_US        (75ULL * 1000000ULL)   /* then coast ... */
#define RIDE_COAST_US       (12ULL * 1000000ULL)   /* ... for this long, then spin up */
#define RIDE_WOBBLE_A1      3.6f
#define RIDE_WOBBLE_T1      23.0f
#define RIDE_WOBBLE_A2      1.4f
#define RIDE_WOBBLE_T2      7.3f

#define CSC_SIM_RPM_MAX     250                    /* cadence meters stop well below this */

/* While the crank is still, keep a measurement flowing this often: a CSC client
 * that sees no traffic at all is entitled to probe the link (the Helm One
 * client ATT-reads the sensor after 8 s of silence). */
#define KEEPALIVE_US        (2ULL * 1000000ULL)
#define STATUS_LOG_US       (5ULL * 1000000ULL)

typedef enum {
    RIDE_SPINUP = 0,
    RIDE_HOLD,
    RIDE_COAST,
} ride_phase_t;

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_tick;

static struct {
    csc_mode_t   mode;
    uint16_t     target;        /* steady cadence / ride cruise, rpm */
    float        rpm;

    ride_phase_t ride_phase;
    uint64_t     phase_us;

    uint64_t     angle;         /* crank angle, 0 .. REV_UREV */
    uint32_t     revs;          /* cumulative revolutions since boot */
    uint16_t     evt_time;      /* last crank event, 1/1024 s */
    uint64_t     evt_us;        /* the same instant in microseconds */

    void       (*tx_cb)(void);
    uint64_t     last_tx_us;
    uint64_t     last_log_us;
    uint64_t     tick_us;

    bool         linked;
    char         peer[24];
    uint32_t     conns;
    uint32_t     ntf_ok;
    uint32_t     ntf_fail;
    uint8_t      battery;
} s;

const char *csc_mode_name(csc_mode_t mode)
{
    switch (mode) {
    case CSC_MODE_STOP:   return "stop";
    case CSC_MODE_STEADY: return "steady";
    case CSC_MODE_RIDE:   return "ride";
    default:              return "?";
    }
}

static float ramp(float cur, float target, float step)
{
    if (cur < target) {
        cur += step;
        if (cur > target) {
            cur = target;
        }
    } else if (cur > target) {
        cur -= step;
        if (cur < target) {
            cur = target;
        }
    }
    return cur;
}

static void update_rpm(uint32_t dt_us)
{
    const float dt = (float)dt_us / 1000000.0f;

    switch (s.mode) {
    case CSC_MODE_STOP:
        s.rpm = ramp(s.rpm, 0.0f, RAMP_STOP_RPM_S * dt);
        break;

    case CSC_MODE_STEADY:
        s.rpm = ramp(s.rpm, (float)s.target, RAMP_CMD_RPM_S * dt);
        break;

    case CSC_MODE_RIDE:
        s.phase_us += dt_us;
        switch (s.ride_phase) {
        case RIDE_SPINUP:
            s.rpm = ramp(s.rpm, (float)s.target, RAMP_SPINUP_RPM_S * dt);
            if (s.rpm >= (float)s.target - 1.0f) {
                s.ride_phase = RIDE_HOLD;
                s.phase_us = 0;
            }
            break;

        case RIDE_HOLD: {
            const float t = (float)s.phase_us / 1000000.0f;
            const float wobble = RIDE_WOBBLE_A1 * sinf(2.0f * (float)M_PI * t / RIDE_WOBBLE_T1) +
                                 RIDE_WOBBLE_A2 * sinf(2.0f * (float)M_PI * t / RIDE_WOBBLE_T2);
            s.rpm = (float)s.target + wobble;
            if (s.phase_us >= RIDE_HOLD_US) {
                s.ride_phase = RIDE_COAST;
                s.phase_us = 0;
            }
            break;
        }

        case RIDE_COAST:
            s.rpm = ramp(s.rpm, 0.0f, RAMP_COAST_RPM_S * dt);
            if (s.rpm <= 0.01f && s.phase_us >= RIDE_COAST_US) {
                s.ride_phase = RIDE_SPINUP;
                s.phase_us = 0;
            }
            break;

        default:
            s.ride_phase = RIDE_SPINUP;
            break;
        }
        break;

    default:
        break;
    }

    if (s.rpm < 0.0f) {
        s.rpm = 0.0f;
    }
}

/* Advance the crank over one tick; returns the number of revolutions completed. */
static int advance_crank(uint64_t tick_start_us, uint32_t dt_us)
{
    const uint32_t rpm = (uint32_t)lrintf(s.rpm);
    if (rpm == 0) {
        return 0;
    }

    const uint64_t adv = (uint64_t)rpm * dt_us;    /* micro-revolutions this tick */
    uint64_t angle = s.angle + adv;
    uint64_t before = s.angle;
    uint64_t last_cross_us = 0;
    int events = 0;

    while (angle >= REV_UREV) {
        /* Interpolate when inside this tick the revolution completed.  Only the
         * first crossing needs it; more than one per tick would need a crank
         * turning faster than 1200 rpm, which the model never reaches. */
        const uint64_t off_us = (events == 0) ? ((REV_UREV - before) * dt_us) / adv : dt_us;
        last_cross_us = tick_start_us + off_us;
        angle -= REV_UREV;
        before = 0;
        events++;
    }

    s.angle = angle;

    if (events > 0) {
        s.revs += (uint32_t)events;
        s.evt_us = last_cross_us;
        s.evt_time = (uint16_t)((last_cross_us * 1024ULL) / 1000000ULL);
        return events;
    }
    return 0;
}

static void tick(void *arg)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    void (*tx)(void) = NULL;
    char line[160];

    line[0] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);

    const uint64_t prev_tick_us = s.tick_us;
    const uint32_t dt_us = (uint32_t)(now - prev_tick_us);
    s.tick_us = now;

    int events = 0;
    if (dt_us > 0) {
        update_rpm(dt_us);
        events = advance_crank(prev_tick_us, dt_us);
    }

    if (s.linked) {
        if (events > 0 || (now - s.last_tx_us) >= KEEPALIVE_US) {
            tx = s.tx_cb;
            s.last_tx_us = now;
        }
        if ((now - s.last_log_us) >= STATUS_LOG_US) {
            s.last_log_us = now;
            snprintf(line, sizeof(line),
                     "link peer=%s mode=%s rpm=%u revs=%lu ntf=%lu err=%lu",
                     s.peer, csc_mode_name(s.mode), (unsigned)lrintf(s.rpm),
                     (unsigned long)s.revs, (unsigned long)s.ntf_ok,
                     (unsigned long)s.ntf_fail);
        }
    }

    xSemaphoreGive(s_lock);

    if (tx != NULL) {
        tx();
    }
    if (line[0] != '\0') {
        ESP_LOGI(TAG, "%s", line);
    }
}

void csc_sim_set_tx_cb(void (*cb)(void))
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.tx_cb = cb;
    xSemaphoreGive(s_lock);
}

void csc_sim_sample(csc_sample_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->revs = s.revs;
    out->evt_time = s.evt_time;
    out->rpm = (uint16_t)lrintf(s.rpm);
    xSemaphoreGive(s_lock);
}

void csc_sim_status(csc_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->mode = s.mode;
    out->rpm = (uint16_t)lrintf(s.rpm);
    out->target_rpm = s.target;
    out->revs = s.revs;
    out->evt_time = s.evt_time;
    out->battery = s.battery;
    out->linked = s.linked;
    memcpy(out->peer, s.peer, sizeof(out->peer));
    out->conns = s.conns;
    out->ntf_ok = s.ntf_ok;
    out->ntf_fail = s.ntf_fail;
    xSemaphoreGive(s_lock);
}

void csc_sim_steady(uint16_t rpm)
{
    if (rpm > CSC_SIM_RPM_MAX) {
        rpm = CSC_SIM_RPM_MAX;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (rpm == 0) {
        s.mode = CSC_MODE_STOP;
    } else {
        s.mode = CSC_MODE_STEADY;
        s.target = rpm;
    }
    const csc_mode_t mode = s.mode;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=%s target=%u rpm", csc_mode_name(mode), (unsigned)rpm);
}

void csc_sim_ride(uint16_t cruise_rpm)
{
    if (cruise_rpm == 0) {
        cruise_rpm = RIDE_CRUISE_DEFAULT;
    }
    if (cruise_rpm > CSC_SIM_RPM_MAX) {
        cruise_rpm = CSC_SIM_RPM_MAX;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool restarted = (s.mode != CSC_MODE_RIDE);
    s.mode = CSC_MODE_RIDE;
    s.target = cruise_rpm;
    if (restarted) {
        s.ride_phase = RIDE_SPINUP;
        s.phase_us = 0;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=ride cruise=%u rpm", (unsigned)cruise_rpm);
}

void csc_sim_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.mode = CSC_MODE_STOP;
    s.target = 0;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=stop");
}

uint8_t csc_sim_battery_get(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint8_t level = s.battery;
    xSemaphoreGive(s_lock);
    return level;
}

void csc_sim_battery_set(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.battery = pct;
    xSemaphoreGive(s_lock);
}

void csc_sim_link_up(const char *peer)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.linked = true;
    s.conns++;
    snprintf(s.peer, sizeof(s.peer), "%s", (peer != NULL) ? peer : "-");
    /* Send the first measurement soon after subscribe rather than waiting for a
     * revolution: a client seeds its baseline from the first packet it gets. */
    s.last_tx_us = 0;
    s.last_log_us = (uint64_t)esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

void csc_sim_link_down(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.linked = false;
    snprintf(s.peer, sizeof(s.peer), "-");
    xSemaphoreGive(s_lock);
}

void csc_sim_count_notify(bool ok)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) {
        s.ntf_ok++;
    } else {
        s.ntf_fail++;
    }
    xSemaphoreGive(s_lock);
}

void csc_sim_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    /* Start riding rather than parked: opening this board's console resets it
     * (the USB bridge pulses EN), so the sim has to be useful the moment it has
     * booted instead of waiting for a command that a busy console might eat. */
    s.mode = CSC_MODE_RIDE;
    s.ride_phase = RIDE_SPINUP;
    s.rpm = 0.0f;
    s.target = RIDE_CRUISE_DEFAULT;
    s.battery = 100;
    s.tick_us = (uint64_t)esp_timer_get_time();
    s.last_tx_us = s.tick_us;
    s.last_log_us = s.tick_us;
    snprintf(s.peer, sizeof(s.peer), "-");

    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "csc-model",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick, TICK_US));
}
