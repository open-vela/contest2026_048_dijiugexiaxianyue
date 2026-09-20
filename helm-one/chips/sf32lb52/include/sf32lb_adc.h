/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/include/sf32lb_adc.h
 *
 * SF32LB52x GPADC. QFN68 pin 20 VBATS is internally wired to software
 * channel 7 (datasheet CH8 / BAT). It is not a GPIO: do not HAL_PIN_Set.
 * CH7 + ANAU EN_VBAT_MON samples the on-chip battery divider; ANAU EN_BG
 * must be set while GPADC runs (datasheet). `am_data` is pack millivolts:
 * factory two-point converts raw to tap mV, then eFuse vbat_mv/vbat_reg
 * undoes the divider (0.5 @ 3.3 V AVDD or 0.3 @ 1.8 V). No eFuse: ×2.01.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_SIFLI_SF32LB52_INCLUDE_SF32LB_ADC_H
#define __VENDOR_SIFLI_SF32LB52_INCLUDE_SF32LB_ADC_H

#include <stdint.h>

#define ADC_CHAN_0             0
#define ADC_CHAN_1             1
#define ADC_CHAN_2             2
#define ADC_CHAN_3             3
#define ADC_CHAN_4             4
#define ADC_CHAN_5             5
#define ADC_CHAN_6             6
#define ADC_CHAN_7             7

/** Internal VBATS / BAT monitor (not PA20). */
#define ADC_CHAN_VBAT          ADC_CHAN_7

/**
 * @brief Init GPADC and register NuttX ADC character device @p devpath.
 *
 * Default channel is VBATS (CH7). `ioctl(ANIOC_TRIGGER)` then `read()` of
 * `struct adc_msg_s` returns pack millivolts in `am_data`.
 * Busy-waits ~300 ms once for analog settle. Do not trigger from IRQ/wdog.
 *
 * @return 0 or a negative errno.
 */
int sf32lb_adc_init(const char *devpath);

#endif /* __VENDOR_SIFLI_SF32LB52_INCLUDE_SF32LB_ADC_H */
