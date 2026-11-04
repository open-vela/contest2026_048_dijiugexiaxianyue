/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-12-21     zylx         first version
 */

#include "board.h"
#include "boot_flash.h"
#include "boot_hw.h"
#include "boot_lcd.h"
#include "boot_msh.h"
#include "sd_nand_drv.h"
#include "secboot.h"

void SystemClock_Config(void)
{
}

#if 0
const unsigned short int pin_array[][3] =
{
    { PAD_PA19, PA19_I2C_UART, 4},
    { PAD_PA18, PA18_I2C_UART, 4},

    { PAD_SA00, MPI1_DIO2,  5},
    { PAD_SA01, MPI1_CS,    5},
    { PAD_SA02, MPI1_DIO1,  5},
    { PAD_SA03, MPI1_DIO2,  5},
    { PAD_SA04, MPI1_CS,    5},
    { PAD_SA07, MPI1_DIO0,  5},
    { PAD_SA08, MPI1_DIO3,  5},
    { PAD_SA09, MPI1_CLK,   5},
    { PAD_SA10, MPI1_DIO3,  5},
    { PAD_SA11, MPI1_DIO0,  5},

    { PAD_PA16, MPI2_CLK, 1},
    { PAD_PA12, MPI2_CS,  1},
    { PAD_PA15, MPI2_DIO0, 1},
    { PAD_PA13, MPI2_DIO1, 1},
    { PAD_PA14, MPI2_DIO2, 1},
    { PAD_PA17, MPI2_DIO3, 1},

    { PAD_PA14, SD1_CLK,  2},
    { PAD_PA15, SD1_CMD,  2},
    { PAD_PA16, SD1_DIO0, 2},
    { PAD_PA17, SD1_DIO1, 2},
    { PAD_PA12, SD1_DIO2, 2},
    { PAD_PA13, SD1_DIO3, 2},
    { PAD_PA23, GPIO_A23, 0},

};

int HAL_PIN_Func2Idx(int pad, pin_function func, int hcpu)
{
    int i;

    for (i = 0; i < sizeof(pin_array) / sizeof(pin_array[0]); i++)
        if (pad == pin_array[i][0] && func == pin_array[i][1])
            return pin_array[i][2];
    return 0;
}
#endif

// Do not use HAL_PIN_Get in bootloader ROM
pin_function HAL_PIN_Idx2Func(int pad, int idx,  int hcpu)
{
    return 0;
}

void board_pinmux_mpi1_puya_base()
{
    HAL_PIN_Set(PAD_SA01, MPI1_CS,   PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA09, MPI1_CLK,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA07, MPI1_DIO0, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA02, MPI1_DIO1, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA10, MPI1_DIO3, PIN_NOPULL, 1);


}

void board_pinmux_mpi1_puya_ext(int is64Mb)
{
    HAL_PIN_Set_Analog(PAD_SA04, 1);
    HAL_PIN_Set_Analog(PAD_SA05, 1);
    HAL_PIN_Set_Analog(PAD_SA06, 1);
    HAL_PIN_Set_Analog(PAD_SA08, 1);
    HAL_PIN_Set_Analog(PAD_SA11, 1);
    HAL_PIN_Set_Analog(PAD_SA12, 1);

    if (is64Mb)
    {
        HAL_PIN_Set_Analog(PAD_SA00, 1);
        HAL_PIN_Set(PAD_SA03, MPI1_DIO2, PIN_PULLUP, 1);
    }
    else
    {
        HAL_PIN_Set_Analog(PAD_SA03, 1);
        HAL_PIN_Set(PAD_SA00, MPI1_DIO2, PIN_PULLUP, 1);
    }
}

void board_pinmux_mpi1_gd()
{
    HAL_PIN_Set(PAD_SA04, MPI1_CS,   PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA09, MPI1_CLK,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_SA11, MPI1_DIO0, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA02, MPI1_DIO1, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_SA00, MPI1_DIO2, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_SA08, MPI1_DIO3, PIN_PULLUP, 1);

    HAL_PIN_Set_Analog(PAD_SA01, 1);
    HAL_PIN_Set_Analog(PAD_SA03, 1);
    HAL_PIN_Set_Analog(PAD_SA05, 1);
    HAL_PIN_Set_Analog(PAD_SA06, 1);
    HAL_PIN_Set_Analog(PAD_SA07, 1);
    HAL_PIN_Set_Analog(PAD_SA10, 1);
    HAL_PIN_Set_Analog(PAD_SA12, 1);

}

void board_pinmux_mpi2()
{
    HAL_PIN_Set(PAD_PA16, MPI2_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA12, MPI2_CS,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA15, MPI2_DIO0, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_PA13, MPI2_DIO1, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_PA14, MPI2_DIO2, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA17, MPI2_DIO3, PIN_PULLUP, 1);
}

void board_pinmux_sd()
{
    HAL_PIN_Set(PAD_PA15, SD1_CMD, PIN_PULLUP, 1);
    HAL_Delay_us(20);   // add a delay before clock setting to avoid wrong cmd happen

    HAL_PIN_Set(PAD_PA14, SD1_CLK,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA16, SD1_DIO0, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA17, SD1_DIO1, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA12, SD1_DIO2, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA13, SD1_DIO3, PIN_PULLUP, 1);
}

void board_pinmux_uart()
{
    HAL_PIN_Set(PAD_PA19, USART1_TXD, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA18, USART1_RXD, PIN_PULLUP, 1);
}

static void board_pinmux_mpi1_none(void)
{
    uint32_t i;

    for (i = 0; i <= 12; i++)
    {
        HAL_PIN_Set_Analog(PAD_SA00 + i, 1);
    }
}

int board_boot_from(void)
{
    uint32_t pid = (hwp_hpsys_cfg->IDR & HPSYS_CFG_IDR_PID_Msk) >> HPSYS_CFG_IDR_PID_Pos;
    int r;

    pid &= 7;
    if (BOOT_SIP_NONE == pid)
        board_pinmux_mpi1_none();

    if (pid == BOOT_SIP_PUYA)
        r = BOOT_FROM_SIP_PUYA;
    else if (pid == BOOT_SIP_GD)
        r = BOOT_FROM_SIP_GD;
    else
#ifdef CFG_BOOTROM
    {
        hwp_pmuc->CR &= ~PMUC_CR_PIN_RET;
        HAL_Delay_us(100);

        // Use external PIN(DIO1,DIO3) to detect NOR/NAND/SD-NAND
        int ext_bond = HAL_GPIO_ReadPin(hwp_gpio1, 17);      // A17,  MPI2_DIO3
        ext_bond |= (HAL_GPIO_ReadPin(hwp_gpio1, 13) << 1);  // A13,  MPI2_DIO1

        if (ext_bond == 0)
            r = BOOT_FROM_NOR;
        else if (ext_bond == 1)
            r = BOOT_FROM_NAND;
        else
        {
            if (__HAL_SYSCFG_GET_REVID() >= HAL_CHIP_REV_ID_A4)
                r = (ext_bond == 2) ? BOOT_FROM_SD : BOOT_FROM_EMMC;
            else
                r = BOOT_FROM_SD;
        }
    }
    HAL_Set_backup(RTC_BACKUP_BOOTOPT, r);
#else
    {
        /* 2nd-stage bootloader. Normally the boot source is whatever the ROM
         * detected via external bonding (saved in RTC backup). When the
         * bootloader is built for a specific storage medium
         * (boot_loader/storage.conf -> build.sh -> BOOT_DEFAULT_FROM_*),
         * force that source here so an SD / eMMC build always loads images
         * from the card regardless of the ROM bonding result. */
#if defined(BOOT_DEFAULT_FROM_SD)
        r = BOOT_FROM_SD;
#elif defined(BOOT_DEFAULT_FROM_EMMC)
        r = BOOT_FROM_EMMC;
#elif defined(BOOT_DEFAULT_FROM_NAND)
        r = BOOT_FROM_NAND;
#elif defined(BOOT_DEFAULT_FROM_NOR)
        r = BOOT_FROM_NOR;
#else
        r = HAL_Get_backup(RTC_BACKUP_BOOTOPT);
#endif
    }
#endif
    return r;
}

#ifndef CFG_BOOTROM
/* XT48/DLL1 need the 48 MHz crystal; bound the wait so a missing crystal
 * cannot turn the boot decision into a WDT reset loop. */
static int boot_hxt48_wait(uint32_t spins)
{
    uint32_t i;

    hwp_hpsys_aon->ACR |= HPSYS_AON_ACR_HXT48_REQ;
    for (i = 0; i < spins; i++)
    {
        if (hwp_hpsys_aon->ACR & HPSYS_AON_ACR_HXT48_RDY)
            return 0;
        if ((i & 0x3FFU) == 0)
            boot_hw_wdt_pet();
    }
    hwp_hpsys_aon->ACR &= ~HPSYS_AON_ACR_HXT48_REQ;
    return -1;
}

/* Charge page / msh stay on HRC48 (48 MHz).  Once the boot decision is made
 * and an image is about to be read, raise HCLK to 240 MHz (DLL1) and
 * re-identify the SD card at the new HCLK.  The SD clock stays at 24 MHz via
 * the NuttX stop/program/release sequence.  Any failure falls back to 48 MHz
 * so the board still boots. */
#ifndef BOOT_CLOCK_FAST_ENABLE
#define BOOT_CLOCK_FAST_ENABLE 1
#endif

void boot_clock_fast(void)
{
    static uint8_t done;
    static const char ok[] = "\r\nclk: fast 240M, sd 24M\r\n";
    static const char nxt[] = "\r\nclk: xt48 not ready, stay 48M\r\n";
    static const char back[] = "\r\nclk: fast failed, back to 48M\r\n";
    int sd_live;

    if (done)
        return;
    done = 1;

#if !BOOT_CLOCK_FAST_ENABLE
    /* Experimental 240 MHz path disabled: fresh SD init at 240 hangs at
     * CMD0.  Keep the two-stage flow at the proven 48 MHz point. */
    return;
#endif

    if (board_boot_src != BOOT_FROM_SD)
        return;

    sd_live = (g_flash_read != NULL);

    boot_uart_tx(hwp_usart1, (uint8_t *)"c1", 2);
    if (boot_hxt48_wait(1000000U) != 0)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)nxt, sizeof(nxt) - 1);
        return;
    }
    boot_uart_tx(hwp_usart1, (uint8_t *)"c2", 2);

    HAL_PMU_EnableDLL(1);
    HAL_Delay_us(100);
    boot_uart_tx(hwp_usart1, (uint8_t *)"c3", 2);

    /* If the card was already identified at 48 MHz, stop SDCLK before HCLK
     * moves.  In the two-stage flow the card is not touched at 48 at all and
     * dfu_flash_init() identifies it fresh at 240 MHz. */
    if (sd_live)
    {
        sdmmc1_clock_suspend();
    }
    boot_uart_tx(hwp_usart1, (uint8_t *)"c4", 2);
    if (HAL_RCC_HCPU_ConfigHCLK(240) != HAL_OK)
    {
        if (sd_live)
        {
            sd_clock_set_hz(24000000U);
        }
        boot_uart_tx(hwp_usart1, (uint8_t *)back, sizeof(back) - 1);
        return;
    }
    boot_uart_tx(hwp_usart1, (uint8_t *)"c5", 2);
    /* HAL_RCC_HCPU_SetDiv() ran before the SYSCLK mux was switched to DLL1,
     * and reading CSR right after the mux write still returns the old HRC48
     * source, so SystemCoreClock/GetHCLKFreq() say 48 MHz.  Use the frequency
     * ConfigHCLK actually programmed: otherwise SysTick LOAD is 5x too small,
     * HAL_GetTick runs 5x fast and every 20 ms SD/DMA timeout is 4 ms real. */
    SystemCoreClock = 240000000U;
    HAL_InitTick(TICK_INT_PRIORITY);
    /* Re-program the time base explicitly: 240000 cycles at HCLK 240 is
     * exactly 1 ms.  Clear the pending exception so the first tick cannot
     * re-enter. */
    SysTick->LOAD = 240000U - 1U;
    SysTick->VAL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
    boot_uart_tx(hwp_usart1, (uint8_t *)"c6", 2);

    if (sd_live)
    {
        /* HCLK changed under a live card: re-identify at 400 kHz and come
         * back at 24 MHz with the clean clock sequence. */
        if (sdmmc1_sdnand() != 1)
        {
            boot_uart_tx(hwp_usart1, (uint8_t *)"cx", 2);
            (void)HAL_RCC_HCPU_ConfigHCLK(48);
            SystemCoreClock = 48000000U;
            HAL_InitTick(TICK_INT_PRIORITY);
            (void)sdmmc1_sdnand();
            boot_lcd_reclock();
            boot_msh_clock_changed();
            boot_uart_tx(hwp_usart1, (uint8_t *)back, sizeof(back) - 1);
            return;
        }
        boot_uart_tx(hwp_usart1, (uint8_t *)"c7", 2);
    }

    boot_lcd_reclock();
    boot_msh_clock_changed();
    boot_uart_tx(hwp_usart1, (uint8_t *)ok, sizeof(ok) - 1);
}

/* Fallback used by the fresh-at-240 SD init: go back to the proven 48 MHz
 * point and let the caller re-identify the card there. */
void boot_clock_back_48(void)
{
    (void)HAL_RCC_HCPU_ConfigHCLK(48);
    SystemCoreClock = 48000000U;
    HAL_InitTick(TICK_INT_PRIORITY);
    boot_lcd_reclock();
    boot_msh_clock_changed();
}
#endif /* !CFG_BOOTROM */

// Boot MPI Power pins
uint32_t g_boot_opt;
void board_flash_power_on()
{
#ifdef CFG_BOOTROM
    if (__HAL_SYSCFG_GET_REVID() >= HAL_CHIP_REV_ID_A4)
    {
        uint32_t delay;
        g_boot_opt = HAL_Get_backup(RTC_BACKUP_BOOTOPT);

        delay = (g_boot_opt & BOOT_PD_Delay_Msk) >> BOOT_PD_Delay_Pos;
        if (delay)
        {
            BSP_GPIO_Set(MPI_POWER_PIN, 0, 1);
            HAL_Delay_us(delay * 1000);
        }

        BSP_GPIO_Set(MPI_POWER_PIN, 1, 1);
        delay = (g_boot_opt & BOOT_PU_Delay_Msk) >> BOOT_PU_Delay_Pos;
        if (delay)
            HAL_Delay_us(delay * 1000);
    }
    else
        hwp_pmuc->PERI_LDO |= PMUC_PERI_LDO_EN_VDD33_LDO2;
    // No longer needed as bootmode delay is 1s.
    // HAL_Delay_us(0);
    // HAL_Delay_us(2000);
#endif
}



