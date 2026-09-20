/****************************************************************************
 * vendor/sifli/chip/sf32lb52/sifli_lowput.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include "bf0_hal.h"
#include "sf32lb_serial.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void arm_lowputc(char ch)
{
    while ((hwp_usart1->ISR & UART_FLAG_TXE) == 0);
    hwp_usart1->TDR = (uint32_t)ch;
}

/****************************************************************************
 * Name: arm_earlyserialinit
 *
 * Description:
 *   Performs the low level USART initialization early in debug so that the
 *   serial console will be available during bootup.  This must be called
 *   before arm_serialinit.
 *
 ****************************************************************************/

/* UART handler declaration */
static UART_HandleTypeDef g_early_uart_handle;

void arm_earlyserialinit(void)
{
    /* The secondary bootloader already fully configured USART1 (pins, clock,
     * 8N1, baud) and it is actively working - every breadcrumb up to here was
     * printed through it at CONFIG_UART_BAUD (1000000). Re-initialising it here
     * is both redundant and harmful in this early context:
     *   - HAL_UART_Init() -> UART_CheckIdleState() busy-waits for TEACK/REACK
     *     with a HAL_GetTick()-based timeout, but interrupts are disabled
     *     (cpsid i) so the tick never advances -> if the flag is slow the wait
     *     never times out and we hang forever.
     * So keep the bootloader's working configuration for the early console.
     * NuttX's full serial driver will (re)configure USART1 properly later,
     * once the interrupt/tick subsystem is up. */
    (void)g_early_uart_handle;
}
