/* Glue between main.c (radio/identity) and cli.c (console). */
#ifndef APP_H
#define APP_H

#include <stdbool.h>

/* Advertising control; implemented in main.c. */
void app_adv_set(bool on);
bool app_adv_on(void);

/* Starts the serial console REPL; implemented in cli.c. */
void cli_start(void);

#endif /* APP_H */
