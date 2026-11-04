/* Serial console for the cadence simulator.
 *
 * The console shares UART0 with the log output on the CH9102 bridge, so the
 * instrument can be driven and watched over the flashing cable alone.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csc_sim.h"
#include "esp_console.h"
#include "gatt_csc.h"

#define RPM_MAX   250
#define RIDE_MIN  20

static int cmd_status(int argc, char **argv)
{
    csc_status_t st;

    csc_sim_status(&st);

    printf("mode    : %s\n", csc_mode_name(st.mode));
    printf("cadence : %u rpm (target %u)\n", (unsigned)st.rpm, (unsigned)st.target_rpm);
    printf("crank   : %lu revs, last event %u (1/1024 s)\n",
           (unsigned long)st.revs, (unsigned)st.evt_time);
    printf("link    : %s peer=%s conns=%lu ntf=%lu err=%lu\n", st.linked ? "up" : "down",
           st.peer, (unsigned long)st.conns, (unsigned long)st.ntf_ok, (unsigned long)st.ntf_fail);
    printf("radio   : advertise=%s\n", app_adv_on() ? "on" : "off");
    printf("battery : %u%%\n", (unsigned)st.battery);

    return 0;
}

static int cmd_rpm(int argc, char **argv)
{
    int rpm;

    if (argc < 2) {
        printf("usage: rpm <0..%u>   (0 = stop pedalling)\n", (unsigned)RPM_MAX);
        return 1;
    }

    rpm = atoi(argv[1]);
    if (rpm < 0 || rpm > RPM_MAX) {
        printf("rpm: out of range 0..%u\n", RPM_MAX);
        return 1;
    }

    csc_sim_steady((uint16_t)rpm);
    printf("cadence -> %d rpm\n", rpm);

    return 0;
}

static int cmd_ride(int argc, char **argv)
{
    int cruise = 0;

    if (argc >= 2) {
        cruise = atoi(argv[1]);
        if (cruise < RIDE_MIN || cruise > RPM_MAX) {
            printf("ride: cruise out of range %u..%u\n", RIDE_MIN, RPM_MAX);
            return 1;
        }
    }

    csc_sim_ride((uint16_t)cruise);
    printf("ride profile running\n");

    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    csc_sim_stop();
    printf("crank stopping\n");
    return 0;
}

static int cmd_battery(int argc, char **argv)
{
    if (argc >= 2) {
        const int pct = atoi(argv[1]);
        if (pct < 0 || pct > 100) {
            printf("battery: out of range 0..100\n");
            return 1;
        }
        csc_sim_battery_set((uint8_t)pct);
        /* Tell a subscribed client about it, like a real gauge would. */
        gatt_csc_notify_battery();
    }

    printf("battery -> %u%%\n", (unsigned)csc_sim_battery_get());
    return 0;
}

static int cmd_adv(int argc, char **argv)
{
    if (argc < 2) {
        printf("advertising: %s\n", app_adv_on() ? "on" : "off");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        app_adv_set(true);
    } else if (strcmp(argv[1], "off") == 0) {
        app_adv_set(false);
    } else {
        printf("usage: adv <on|off>\n");
        return 1;
    }

    printf("advertising: %s\n", app_adv_on() ? "on" : "off");
    return 0;
}

static const esp_console_cmd_t s_cmds[] = {
    {
        .command = "status",
        .help = "show mode, cadence, link and counters",
        .func = cmd_status,
    }, {
        .command = "rpm",
        .help = "rpm <0..250>: hold a fixed cadence (0 stops)",
        .func = cmd_rpm,
    }, {
        .command = "ride",
        .help = "ride [20..250]: run the realistic riding profile",
        .func = cmd_ride,
    }, {
        .command = "stop",
        .help = "stop pedalling (cadence -> 0)",
        .func = cmd_stop,
    }, {
        .command = "battery",
        .help = "battery [0..100]: show or set the simulated battery",
        .func = cmd_battery,
    }, {
        .command = "adv",
        .help = "adv <on|off>: start or stop connectable advertising",
        .func = cmd_adv,
    },
};

void cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    size_t i;

    repl_cfg.prompt = "csc>";
    repl_cfg.max_cmdline_length = 128;
    repl_cfg.task_stack_size = 4096;

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    for (i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&s_cmds[i]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
