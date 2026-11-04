/* Includes ------------------------------------------------------------------*/
#include <rtconfig.h>
#include "bf0_hal.h"
#include "board.h"
#include "boot_hw.h"
#include "string.h"

__attribute__((weak)) void boot_uart_idle(void)
{
}

void boot_uart_tx(USART_TypeDef *uart, uint8_t *data, int len)
{
    int i;

    for (i = 0; i < len; i++)
    {
        while ((uart->ISR & UART_FLAG_TXE) == 0);
        uart->TDR = (uint32_t)data[i];
        if ((i & 7) == 7)
        {
            boot_uart_idle();
        }
    }
}

/* boot_clock_fast() calls HAL_InitTick() after raising HCLK to 240 MHz.  That
 * enables the SysTick interrupt, but the startup file only provides the weak
 * SysTick_Handler == Default_Handler (infinite loop).  The first tick then
 * hangs the bootloader and the WDT resets ~5 s later -- which looked exactly
 * like "SD CMD0 never completes at 240 MHz".  Keep the HAL tick alive. */
void SysTick_Handler(void)
{
    /* Reading CTRL clears COUNTFLAG; clear the NVIC pending bit too so the
     * exception cannot re-enter before the next 1 ms period. */
    (void)SysTick->CTRL;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    HAL_IncTick();
}

#define MAX_RETRY 3
void boot_error(unsigned char code)
{
    int retry;

    boot_uart_tx(hwp_usart1, &code, 1);

    retry = hwp_pmuc->CAU_RSVD & MAX_RETRY;
    retry++;
    if (retry <= MAX_RETRY)
    {
        hwp_pmuc->CAU_RSVD &= ~MAX_RETRY;
        hwp_pmuc->CAU_RSVD |= retry;
        HAL_PMU_Reboot();
    }
    else
    {
        while (1);
    }
}

/**
* Initializes the Global MSP.
*/
#ifdef TARMAC
    #define BOOT_MODE_DELAY 1000
#else
    #define BOOT_MODE_DELAY 1000000
#endif
void HAL_MspInit(void)
{
    boot_hw_wdt_pet();
#ifdef CFG_BOOTROM
    char *boot_tag = "SFBL\n";
    boot_uart_tx(hwp_usart1, (uint8_t *)boot_tag, strlen(boot_tag));
    if (__HAL_SYSCFG_GET_REVID() < HAL_CHIP_REV_ID_A4 ||
            (hwp_pmuc->CR & (PMUC_CR_HIBER_EN | PMUC_CR_REBOOT)) == 0)
    {
        HAL_Delay_us(BOOT_MODE_DELAY);      // Wait for boot_mode options.
    }
#endif
}

void mpu_config(void)
{
    // Do nothing
}

void cache_enable(void)
{
    // Do nothing
}

static void fault_spin(const char *s)
{
    boot_uart_tx(hwp_usart1, (uint8_t *)s, (int)strlen(s));
    while (1)
    {
    }
}

void HardFault_Handler(void)
{
    fault_spin("FAULT HardFault\r\n");
}

void MemManage_Handler(void)
{
    fault_spin("FAULT MemManage\r\n");
}

void BusFault_Handler(void)
{
    fault_spin("FAULT BusFault\r\n");
}

void UsageFault_Handler(void)
{
    fault_spin("FAULT UsageFault\r\n");
}

