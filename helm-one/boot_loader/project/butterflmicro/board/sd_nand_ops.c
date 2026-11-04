#include <stdio.h>
#include <string.h>
#include "sd_nand_drv.h"
#include "boot_hw.h"
#include "secboot.h"
#include "dma_config.h"

static uint8_t  wire_mode = 1;  //0 for 1-wire mode, 1 for 4-wire mode
static uint8_t  sdsc = 1; //0 for sdhc/sdxc, 1 for sdsc
static uint64_t g_sd_card_bytes;
static uint8_t  g_sd_up;
static uint8_t  g_dat0_poll;

#define SD_ID_CLOCK_HZ   400000U
#define SD_TRAN_CLOCK_HZ 24000000U

/* Same sequence as the NuttX driver: STOP_CLK, program DIV while stopped,
 * wait, then release.  A divider write while SDCLK runs produces a residual
 * pulse that can wedge the card -- especially after HCLK changes. */
static uint32_t sd_clock_div(uint32_t hz)
{
    /* SDMMC1 is fed by HCLK (verified: PCLK2-based divider makes data
     * transfers corrupt at 240 MHz).  Keep HCLK as the divider base. */
    uint32_t clk = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
    uint32_t div;

    if (clk == 0)
        clk = 48000000U;
    div = (clk + hz - 1U) / hz;

    if (div < 2U)
        div = 2U;
    if (div > 8192U)
        div = 8192U;
    return div;
}

void sd_clock_set_hz(uint32_t hz)
{
    uint32_t div = sd_clock_div(hz);

    hwp_sdmmc1->CLKCR = SD_CLKCR_STOP_CLK;
    HAL_Delay_us(10);
    hwp_sdmmc1->CLKCR = ((div - 1U) << SD_CLKCR_DIV_Pos) |
                        SD_CLKCR_VOID_FIFO_ERROR | SD_CLKCR_STOP_CLK;
    HAL_Delay_us(10);
    hwp_sdmmc1->CLKCR &= ~SD_CLKCR_STOP_CLK;
    HAL_Delay_us(10);
}

void sdmmc1_clock_suspend(void)
{
    if (!g_sd_up)
        return;
    hwp_sdmmc1->CLKCR = SD_CLKCR_STOP_CLK;
    HAL_Delay_us(10);
}

#if BOOT_SD_DMA_MULTI
#define SD_MULTI_MAX_BYTES 2048U
static uint8_t sd_dma_buf[SD_MULTI_MAX_BYTES] __attribute__((aligned(32)));
static DMA_HandleTypeDef s_sd_dma;
static uint8_t s_sd_dma_ok;

static int sd_dma_init(void)
{
    if (s_sd_dma_ok)
        return 0;

    HAL_RCC_EnableModule(RCC_MOD_DMAC1);
    memset(&s_sd_dma, 0, sizeof(s_sd_dma));
    s_sd_dma.Instance                 = SDMMC1_DMA_INSTANCE;
    s_sd_dma.Init.Request             = SDMMC1_DMA_REQUEST;
    s_sd_dma.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_sd_dma.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_sd_dma.Init.MemInc              = DMA_MINC_ENABLE;
    s_sd_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    s_sd_dma.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    s_sd_dma.Init.Mode                = DMA_NORMAL;
    s_sd_dma.Init.Priority            = DMA_PRIORITY_LOW;
    s_sd_dma.Init.BurstSize           = 1;
    s_sd_dma.Init.IrqPrio             = SDMMC1_DMA_IRQ_PRIO;

    if (HAL_DMA_Init(&s_sd_dma) != HAL_OK)
        return -1;
    s_sd_dma_ok = 1;
    return 0;
}
#endif

static uint8_t mmcsd_parse_csd(uint32_t *resp);

uint8_t sdmmc1_sdnand()
{
    uint8_t test_result = 1;
    uint8_t  rsp_idx;
    uint32_t rsp_arg[4];
    uint8_t  cmd_result;
    uint8_t  ccs;
    uint16_t rca;
    uint32_t cmd_arg;
    uint32_t cid[4];

    //debug_print("SDMMC1 sd case start!\n");

    //initialize sdmmc host
    sd1_init();
    /* Match the NuttX HAL_SDMMC_INIT register state, then set the
     * identification clock with the stop/program/release sequence. */
    hwp_sdmmc1->DCR = 0x1ffU << SD_DCR_BLOCK_SIZE_Pos;
    hwp_sdmmc1->CLKCR |= SD_CLKCR_VOID_FIFO_ERROR;
    hwp_sdmmc1->SR = 0x3FFFFU;
    hwp_sdmmc1->IER = 0;
    hwp_sdmmc1->CDR &= ~SD_CDR_ITIMING_SEL;
    sd_clock_set_hz(SD_ID_CLOCK_HZ);
    hwp_sdmmc1->IER = 0; //mask sdmmc interrupt
    hwp_sdmmc1->TOR = 0x00100000; // set timeout for 400K about 2.6s

    // add a delay after clock set, at least 74 SD clock
    // need wait more than 200ms for 400khz
    HAL_Delay_us(500);

    rca = 0x0;

    //initialize sd card
    boot_uart_tx(hwp_usart1, (uint8_t *)"s0", 2);
    cmd_result = sd1_send_cmd(0, 0); //CMD0
    if (cmd_result != SD_SUCCESS)
    {
        return '0';
    }

    //set sd_req and wait for sd_busy before access sd in normal mode
    hwp_sdmmc1->CASR = SD_CASR_SD_REQ;
    {
        uint32_t spin = 0;
        uint32_t limit = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU) / 8U;

        if (limit < 1000000U)
            limit = 1000000U;
        while ((hwp_sdmmc1->CASR & SD_CASR_SD_BUSY) == 0)
        {
            if ((++spin & 0x3FFFU) == 0)
            {
                boot_hw_wdt_pet();
                if (spin > limit)
                {
                    return 'B';
                }
            }
        }
    }

    //start card identification
    //CMD8
    HAL_Delay_us(20);
    cmd_arg = 0x000001aa; //VHS=1
    boot_uart_tx(hwp_usart1, (uint8_t *)"s1", 2);
    cmd_result = sd1_send_cmd(8, cmd_arg); //CMD8
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD8 TIMEOUT!\n");
        test_result = '8';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD8 CRC ERR!\n");
        test_result = '8';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    if ((rsp_idx != 0x8) || (rsp_arg[0] != 0x1aa))
    {
        //debug_print("CMD8 RSP ERR!\n");
        test_result = '8';
        //HAL_ASSERT(0);
        return test_result;
    }

    //ACMD41
    boot_uart_tx(hwp_usart1, (uint8_t *)"s2", 2);
    cmd_arg = 0x40ff8000;
    while (1) //wait for card busy status
    {
        boot_hw_wdt_pet();
        HAL_Delay_us(20);
        cmd_result = sd1_send_acmd(41, cmd_arg, rca); //CMD55+ACMD41
        if (cmd_result == SD_TIMEOUT)
        {
            //debug_print("ACMD41 TIMEOUT!\n");
            test_result = '4';
            //HAL_ASSERT(0);
            return test_result; //CMD 41
        }
        sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
        if ((rsp_arg[0] & 0x80000000) != 0)
        {
            break; //card power up done
        }
        HAL_Delay_us(2); //add some delay
    }
    ccs = (rsp_arg[0] >> 30) & 0x1;

    //CMD2
    HAL_Delay_us(20);
    cmd_arg = 0x0;
    boot_uart_tx(hwp_usart1, (uint8_t *)"s4", 2);
    cmd_result = sd1_send_cmd(2, cmd_arg); //CMD2
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD2 TIMEOUT!\n");
        test_result = '2';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD2 CRC ERR!\n");
        test_result = '2';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &cid[3], &cid[2], &cid[1], &cid[0]);

    //CMD3
    boot_uart_tx(hwp_usart1, (uint8_t *)"s5", 2);
    HAL_Delay_us(20);
    cmd_arg = 0x0;
    cmd_result = sd1_send_cmd(3, cmd_arg); //CMD3
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD3 TIMEOUT!\n");
        test_result = '3';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD3 CRC ERR!\n");
        test_result = '3';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    rca = rsp_arg[0] >> 16;
    if (rsp_idx != 0x3)
    {
        //debug_print("CMD3 RSP ERR!\n");
        test_result = '3';
        //HAL_ASSERT(0);
        return test_result;
    }

    HAL_Delay_us(20);
    cmd_arg = rca << 16;
    boot_uart_tx(hwp_usart1, (uint8_t *)"s6", 2);
    cmd_result = sd1_send_cmd(9, cmd_arg); //CMD9
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD9 TIMEOUT!\n");
        test_result = '9';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD9 CRC ERR!\n");
        test_result = '9';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    {
        // FOR R2, it need 128 bits response, high/low words should switch.
        // least 8 bit has been removed, so need fill 8 bits at least bits
        uint32_t temp;
        // switch for [0] as highest
        temp = rsp_arg[0];
        rsp_arg[0] = rsp_arg[3];
        rsp_arg[3] = temp;
        temp = rsp_arg[1];
        rsp_arg[1] = rsp_arg[2];
        rsp_arg[2] = temp;

        // << 8
        rsp_arg[0] = (rsp_arg[0] << 8) | (rsp_arg[1] >> 24);
        rsp_arg[1] = (rsp_arg[1] << 8) | (rsp_arg[2] >> 24);
        rsp_arg[2] = (rsp_arg[2] << 8) | (rsp_arg[3] >> 24);
        rsp_arg[3] = (rsp_arg[3] << 8);
    }

    uint8_t csd_struct = mmcsd_parse_csd(&rsp_arg[0]);
    if (csd_struct == 0)
    {
        //debug_print("SDSC card!\n");
        sdsc = 1;
    }
    else if (csd_struct == 1)
    {
        //debug_print("SDHC card!\n");
        sdsc = 0;
    }
    else
    {
        //debug_print("SD card invalid csd structure !\n");
        test_result = 'T';
        //HAL_ASSERT(0);
        return test_result; // structure Type fail
    }
    //debug_print("SD card identification done!\n");

    boot_uart_tx(hwp_usart1, (uint8_t *)"s7", 2);
    sd_clock_set_hz(SD_TRAN_CLOCK_HZ);
    hwp_sdmmc1->TOR = 0x02000000; // set timeout for 24M about 1.4s
    hwp_sdmmc1->CDR = SD_CDR_ITIMING_SEL | (0 << SD_CDR_ITIMING_Pos);

    //start card transfer
    //CMD7 (SELECT_CARD)
    HAL_Delay_us(20);
    cmd_arg = (uint32_t)rca << 16;
    boot_uart_tx(hwp_usart1, (uint8_t *)"s8", 2);
    cmd_result = sd1_send_cmd(7, cmd_arg);
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD7 TIMEOUT!\n");
        test_result = '7';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD7 CRC ERR!\n");
        test_result = '7';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    if (rsp_idx != 7)
    {
        //debug_print("CMD7 RSP ERR!\n");
        test_result = '7';
        //HAL_ASSERT(0);
        return test_result;
    }

    //ACMD6
    HAL_Delay_us(20);
    cmd_arg = wire_mode ? 2 : 0; //select 4-wire mode or 1-wire mode
    boot_uart_tx(hwp_usart1, (uint8_t *)"s9", 2);
    cmd_result = sd1_send_acmd(6, cmd_arg, rca); //CMD55+ACMD6
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("ACMD6 TIMEOUT!\n");
        test_result = '6';
        //HAL_ASSERT(0);
        return test_result;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("ACMD6 CRC ERR!\n");
        test_result = '6';
        //HAL_ASSERT(0);
        return test_result;
    }

    sd1_read(wire_mode, 1); //4 wire mode,8 blocks

    //CMD17 (READ_SINGLE_BLOCK)
    HAL_Delay_us(20);
    cmd_arg = 0; //start data address
    cmd_result = sd1_send_cmd(17, cmd_arg);
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD17 TIMEOUT!\n");
        test_result = 'R';
        //HAL_ASSERT(0);
        return test_result; // Read command
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD17 CRC ERR!\n");
        test_result = 'R';
        //HAL_ASSERT(0);
        return test_result;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg[0], &rsp_arg[1], &rsp_arg[2], &rsp_arg[3]);
    if (rsp_idx != 17)
    {
        //debug_print("CMD17 RSP ERR!\n");
        test_result = 'R';
        //HAL_ASSERT(0);
        return test_result;
    }

    //wait read data
    hwp_sdmmc1->SR = 0xffffffff; //clear sdmmc interrupts
    hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;
    cmd_result = sd1_wait_read();  //wait sdmmc interrupt
    if (hwp_sdmmc1->SR & SD_SR_DATA_TIMEOUT)
    {
        //debug_print("DATA READ TIMEOUT!\n");
        test_result = 'O';
        //HAL_ASSERT(0);
        return test_result; // read time Out
    }
    if (hwp_sdmmc1->SR & SD_SR_DATA_CRC)
    {
        //debug_print("READ CRC ERR!\n");
        test_result = 'D';
        //HAL_ASSERT(0);
        return test_result; // Data error
    }

    /* Idle DAT0 must read high.  If DSR[0] is stuck, skip later polls. */
    g_dat0_poll = (hwp_sdmmc1->DSR & 1u) != 0;
    g_sd_up = (test_result == TEST_PASS) ? 1 : 0;
    return test_result;
}

static void sd_prepare(void)
{
    hwp_sdmmc1->DCR = 0;
    hwp_sdmmc1->IER = 0;
    sd_wait_idle();
}

static int sd_read_data_fifo(uint64_t addr, uint8_t *data, uint32_t len)
{
    uint8_t  rsp_idx;
    uint8_t test_result = TEST_PASS;
    int i;
    uint32_t cmd_result;
    uint32_t cmd_arg;
    uint32_t *buf = (uint32_t *)data;
    uint32_t rsp_arg1, rsp_arg2, rsp_arg3, rsp_arg4;

    sd_prepare();
    sd1_read(wire_mode, 1);

    //CMD17 (READ_SINGLE_BLOCK)
    HAL_Delay_us(20);
    cmd_arg = sdsc ? (uint32_t)addr : (uint32_t)(addr >> 9); //start data address
    cmd_result = sd1_send_cmd(17, cmd_arg);
    if (cmd_result == SD_TIMEOUT)
    {
        //debug_print("CMD17 TIMEOUT!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }
    else if (cmd_result == SD_CRCERR)
    {
        //debug_print("CMD17 CRC ERR!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg1, &rsp_arg2, &rsp_arg3, &rsp_arg4);
    if (rsp_idx != 17)
    {
        //debug_print("CMD17 RSP ERR!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }

    //wait for dma read data
    hwp_sdmmc1->SR = 0xffffffff; //clear sdmmc interrupts
    hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;
    cmd_result = sd1_wait_read();  //wait sdmmc interrupt
    if (hwp_sdmmc1->SR & SD_SR_DATA_TIMEOUT)
    {
        //debug_print("DATA READ TIMEOUT!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }
    if (hwp_sdmmc1->SR & SD_SR_DATA_CRC)
    {
        //debug_print("READ CRC ERR!\n");
        test_result = TEST_FAIL;
        //HAL_ASSERT(0);
        return 0;
    }

    for (i = 0; i < len / 4; i++)
    {
        *buf = hwp_sdmmc1->FIFO;
        buf++;
    }

    if (test_result == TEST_FAIL)
    {
        //debug_print("Read page fail!\n");
        return 0;
    }

    return len;
}

/* Experimental: one 512-byte block via SD FIFO -> memory DMA.  This removes
 * the per-word FIFO polling that dominates the 2.5 s app read.  Any failure
 * disables the path for the rest of the boot and falls back to the FIFO. */
#define SD_DMA_BLOCK_BYTES 512U
static uint8_t sd_dma_blk_buf[SD_DMA_BLOCK_BYTES] __attribute__((aligned(32)));
static DMA_HandleTypeDef s_sd_dma_blk;
static uint8_t s_sd_dma_blk_ready;
static uint8_t s_sd_dma_blk_ok = 1;
static uint8_t s_sd_dma_blk_dbg;

static int sd_dma_blk_init(void)
{
    if (s_sd_dma_blk_ready)
        return 0;

    HAL_RCC_EnableModule(RCC_MOD_DMAC1);
    memset(&s_sd_dma_blk, 0, sizeof(s_sd_dma_blk));
    s_sd_dma_blk.Instance                 = SDMMC1_DMA_INSTANCE;
    s_sd_dma_blk.Init.Request             = SDMMC1_DMA_REQUEST;
    s_sd_dma_blk.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_sd_dma_blk.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_sd_dma_blk.Init.MemInc              = DMA_MINC_ENABLE;
    s_sd_dma_blk.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    s_sd_dma_blk.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    s_sd_dma_blk.Init.Mode                = DMA_NORMAL;
    s_sd_dma_blk.Init.Priority            = DMA_PRIORITY_LOW;
    s_sd_dma_blk.Init.BurstSize           = 1;
    s_sd_dma_blk.Init.IrqPrio             = SDMMC1_DMA_IRQ_PRIO;

    if (HAL_DMA_Init(&s_sd_dma_blk) != HAL_OK)
        return -1;
    s_sd_dma_blk_ready = 1;
    return 0;
}

static int sd_dma_blk_wait(uint32_t timeout_ms)
{
    uint32_t tc = DMA_FLAG_TC1 << (s_sd_dma_blk.ChannelIndex & 0x1cU);
    uint32_t te = DMA_FLAG_TE1 << (s_sd_dma_blk.ChannelIndex & 0x1cU);
    uint32_t err = SD_SR_DATA_TIMEOUT | SD_SR_DATA_CRC | SD_SR_FIFO_OVERRUN;
    uint32_t t0 = HAL_GetTick();

    while ((s_sd_dma_blk.DmaBaseAddress->ISR & tc) == 0)
    {
        if (s_sd_dma_blk.DmaBaseAddress->ISR & te)
        {
            (void)HAL_DMA_Abort(&s_sd_dma_blk);
            return -1;
        }
        if (hwp_sdmmc1->SR & err)
        {
            (void)HAL_DMA_Abort(&s_sd_dma_blk);
            return -1;
        }
        if ((HAL_GetTick() - t0) > timeout_ms)
        {
            (void)HAL_DMA_Abort(&s_sd_dma_blk);
            return -1;
        }
        boot_hw_wdt_pet();
    }
    return 0;
}

static int sd_read_data_dma(uint64_t addr, uint8_t *data, uint32_t len)
{
    uint8_t rsp_idx;
    uint8_t cmd_result;
    uint32_t cmd_arg;
    uint32_t r0, r1, r2, r3;

    if (len != SD_DMA_BLOCK_BYTES || data == NULL)
        return 0;
    if (sd_dma_blk_init() != 0)
        return 0;

    sd_prepare();
    sd1_read(wire_mode, 1);
    HAL_Delay_us(20);

    if (HAL_DMA_Start(&s_sd_dma_blk, (uint32_t)&hwp_sdmmc1->FIFO,
                      (uint32_t)sd_dma_blk_buf, len / 4U) != HAL_OK)
    {
        hwp_sdmmc1->DCR = 0;
        return 0;
    }

    cmd_arg = sdsc ? (uint32_t)addr : (uint32_t)(addr >> 9);
    cmd_result = sd1_send_cmd(17, cmd_arg);
    if (cmd_result != SD_SUCCESS)
        goto fail;
    sd1_get_rsp(&rsp_idx, &r0, &r1, &r2, &r3);
    if (rsp_idx != 17)
        goto fail;
    if (sd_dma_blk_wait(20) != 0)
        goto fail;

    memcpy(data, sd_dma_blk_buf, len);
    sd_wait_idle();
    return (int)len;

fail:
    hwp_sdmmc1->DCR = 0;
    (void)HAL_DMA_Abort(&s_sd_dma_blk);
    return 0;
}

int sd_read_data(uint64_t addr, uint8_t *data, uint32_t len)
{
    if (len == SD_DMA_BLOCK_BYTES && data != NULL && s_sd_dma_blk_ok)
    {
        if (sd_read_data_dma(addr, data, len) == (int)len)
        {
            if (!s_sd_dma_blk_dbg)
            {
                s_sd_dma_blk_dbg = 1;
                boot_uart_tx(hwp_usart1, (uint8_t *)"D", 1);
            }
            return (int)len;
        }
        s_sd_dma_blk_ok = 0;
        boot_uart_tx(hwp_usart1, (uint8_t *)"X", 1);
    }
    return sd_read_data_fifo(addr, data, len);
}

static __inline uint32_t GET_BITS(uint32_t *resp, uint32_t  start, uint32_t  size)
{
    const int32_t __size = size;
    const uint32_t __mask = (__size < 32 ? 1 << __size : 0) - 1;
    const int32_t __off = 3 - ((start) / 32);
    const int32_t __shft = (start) & 31;
    uint32_t __res;

    __res = resp[__off] >> __shft;
    if (__size + __shft > 32)
        __res |= resp[__off - 1] << ((32 - __shft) % 32);

    return __res & __mask;
}

static uint8_t mmcsd_parse_csd(uint32_t *resp)
{
    uint32_t csd_structure = GET_BITS(resp, 126, 2);
    uint32_t c_size;
    uint32_t read_bl_len;
    uint32_t c_mult;

    if (csd_structure == 0)
    {
        read_bl_len = GET_BITS(resp, 80, 4);
        c_size = GET_BITS(resp, 62, 12);
        c_mult = GET_BITS(resp, 47, 3);
        g_sd_card_bytes = ((uint64_t)c_size + 1ull) << (c_mult + 2 + read_bl_len);
    }
    else
    {
        c_size = GET_BITS(resp, 48, csd_structure >= 2 ? 28 : 22);
        g_sd_card_bytes = ((uint64_t)c_size + 1ull) * 512ull * 1024ull;
    }
    return (uint8_t)csd_structure;
}

uint64_t sd_card_bytes(void)
{
    return g_sd_card_bytes;
}

int sd_write_data(uint64_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t rsp_idx;
    uint32_t cmd_result;
    uint32_t cmd_arg;
    uint32_t rsp_arg1, rsp_arg2, rsp_arg3, rsp_arg4;
    const uint32_t *buf = (const uint32_t *)data;
    int i;

    if (len != SD_BLOCK_SIZE)
    {
        return 0;
    }

    sd_prepare();
    HAL_Delay_us(20);
    cmd_arg = sdsc ? (uint32_t)addr : (uint32_t)(addr >> 9);
    cmd_result = sd1_send_cmd(24, cmd_arg);
    if (cmd_result != SD_SUCCESS)
    {
        return 0;
    }
    sd1_get_rsp(&rsp_idx, &rsp_arg1, &rsp_arg2, &rsp_arg3, &rsp_arg4);
    if (rsp_idx != 24)
    {
        return 0;
    }

    sd1_write(wire_mode, 1);
    hwp_sdmmc1->SR = 0xffffffff;
    hwp_sdmmc1->IER = SD_IER_DATA_DONE_MASK;
    for (i = 0; i < (int)(SD_BLOCK_SIZE / 4); i++)
    {
        hwp_sdmmc1->FIFO = buf[i];
    }
    if (sd1_wait_write() != SD_SUCCESS)
    {
        return 0;
    }
    /* DATA_DONE is the host TX engine.  The card still holds DAT0 low while
     * it programs; reading OVNX on another region before that finishes can
     * interrupt the KV LittleFS commit (especially /fw on KV_REGION).
     */
    sd_wait_idle();
    return (int)len;
}

void sd_wait_idle(void)
{
    unsigned n;

    if (!g_sd_up)
    {
        return;
    }

    /* Host data engine (TXACT). */
    for (n = 0; n < 200000u; n++)
    {
        if ((hwp_sdmmc1->SR & SD_SR_DATA_BUSY) == 0)
        {
            break;
        }
        boot_hw_wdt_pet();
        HAL_Delay_us(10);
    }

    if (!g_dat0_poll)
    {
        return;
    }
    /* Card programming busy on DAT0.  2 s @ 10 us. */
    for (n = 0; n < 200000u; n++)
    {
        if ((hwp_sdmmc1->DSR & 1u) != 0)
        {
            return;
        }
        boot_hw_wdt_pet();
        HAL_Delay_us(10);
    }
}

#if BOOT_SD_DMA_MULTI
static int sd_dma_wait(uint32_t timeout_ms)
{
    uint32_t tc = DMA_FLAG_TC1 << (s_sd_dma.ChannelIndex & 0x1cU);
    uint32_t te = DMA_FLAG_TE1 << (s_sd_dma.ChannelIndex & 0x1cU);
    uint32_t err = SD_SR_DATA_TIMEOUT | SD_SR_DATA_CRC | SD_SR_FIFO_OVERRUN;
    uint32_t t0 = HAL_GetTick();

    while ((s_sd_dma.DmaBaseAddress->ISR & tc) == 0)
    {
        if (s_sd_dma.DmaBaseAddress->ISR & te)
        {
            (void)HAL_DMA_Abort(&s_sd_dma);
            return -1;
        }
        if (hwp_sdmmc1->SR & err)
        {
            (void)HAL_DMA_Abort(&s_sd_dma);
            return -1;
        }
        if ((HAL_GetTick() - t0) > timeout_ms)
        {
            (void)HAL_DMA_Abort(&s_sd_dma);
            return -1;
        }
        boot_hw_wdt_pet();
    }
    return 0;
}

int sd_read_multi(uint64_t addr, uint8_t *data, uint32_t len)
{
    uint32_t nblocks;
    uint32_t arg;
    uint32_t dcr;
    uint32_t r0, r1, r2, r3;
    uint8_t rsp_idx;
    uint8_t cmd_result;
    static uint8_t entry_dbg;

    if (!entry_dbg)
    {
        entry_dbg = 1;
        boot_uart_tx(hwp_usart1, (uint8_t *)"E", 1);
    }

    if (!g_sd_up || data == NULL || len == 0 || len > SD_MULTI_MAX_BYTES ||
        (len & 0x1FFU) != 0)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"0", 1);
        return -1;
    }
    nblocks = len / 512U;
    if (nblocks < 2U)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"1", 1);
        return -1;
    }
    if (sd_dma_init() != 0)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"i", 1);
        return -1;
    }

    sd_wait_idle();

    hwp_sdmmc1->DCR = 0;
    hwp_sdmmc1->SR = 0xffffffff;
    hwp_sdmmc1->DLR = len - 1U;
    dcr = (512U - 1U) << SD_DCR_BLOCK_SIZE_Pos;
    dcr |= (wire_mode << SD_DCR_WIRE_MODE_Pos) | SD_DCR_R_WN;
    dcr |= SD_DCR_TRAN_DATA_EN | SD_DCR_DATA_START;
    hwp_sdmmc1->DCR = dcr;

    if (HAL_DMA_Start(&s_sd_dma, (uint32_t)&hwp_sdmmc1->FIFO,
                      (uint32_t)sd_dma_buf, len / 4U) != HAL_OK)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"a", 1);
        hwp_sdmmc1->DCR = 0;
        return -1;
    }

    arg = sdsc ? (uint32_t)addr : (uint32_t)(addr >> 9);
    cmd_result = sd1_send_cmd(18, arg);
    if (cmd_result != SD_SUCCESS)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"c", 1);
        goto fail;
    }
    sd1_get_rsp(&rsp_idx, &r0, &r1, &r2, &r3);
    if (rsp_idx != 18)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"r", 1);
        goto fail;
    }
    if (sd_dma_wait(20) != 0)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"d", 1);
        goto fail;
    }

    sd_wait_idle();
    cmd_result = sd1_send_cmd(12, 0);
    if (cmd_result != SD_SUCCESS)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"x", 1);
        goto fail;
    }
    sd_wait_idle();

    memcpy(data, sd_dma_buf, len);
    return 0;

fail:
    hwp_sdmmc1->DCR = 0;
    (void)HAL_DMA_Abort(&s_sd_dma);
    (void)sd1_send_cmd(12, 0);
    sd_wait_idle();
    return -1;
}
#endif /* BOOT_SD_DMA_MULTI */

void sd_release(void)
{
    if (!g_sd_up)
    {
        return;
    }

    hwp_sdmmc1->DCR = 0;
    hwp_sdmmc1->IER = 0;
    hwp_sdmmc1->SR = 0xffffffff;
    sd_wait_idle();
    (void)sd1_send_cmd(0, 0);
    HAL_Delay_us(1000);
    hwp_sdmmc1->CLKCR = 0;
    hwp_hpsys_rcc->ENR2 &= ~HPSYS_RCC_ENR2_SDMMC1;
    g_sd_up = 0;
}

