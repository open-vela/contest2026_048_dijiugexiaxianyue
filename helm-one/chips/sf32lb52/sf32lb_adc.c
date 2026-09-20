/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/param.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>
#include <nuttx/mutex.h>

#include "bf0_hal.h"
#include "bf0_hal_adc.h"
#include "bf0_sys_cfg.h"
#include "gpadc.h"
#include "register.h"

#include "sf32lb_adc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ADC_SAMPLE_MAX            8
#define ADC_QUICK_COUNT           6
#define ADC_QUICK_GAP_US          200u

/* 片内分压：3.3 V AVDD 标称 0.5（k≈2.01），1.8 V AVDD 标称 0.3（k≈3.33）。
 * 以 eFuse vbat_mv/vbat_reg 为准；本板 ATE 为 ~3.30。无校准才用 2.01。 */
#define ADC_VBAT_FACTOR_DEFAULT   2.01f
#define ADC_VBAT_FACTOR_MIN       1.50f
#define ADC_VBAT_FACTOR_MAX       4.00f
#define ADC_ATE_FREQ_HZ           240000u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct adc_info_s
{
    ADC_HandleTypeDef adc_handle;
    const struct adc_callback_s *cb;
    uint8_t channel;
    float adc_vol_offset;
    float adc_vol_ratio;
    float adc_vbat_factor;
    uint32_t adc_thd_reg;
    uint32_t ref;
    uint8_t initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int adc_bind(struct adc_dev_s *dev,
                    const struct adc_callback_s *callback);
static void adc_reset(struct adc_dev_s *dev);
static int adc_setup(struct adc_dev_s *dev);
static void adc_shutdown(struct adc_dev_s *dev);
static void adc_rxint(struct adc_dev_s *dev, bool enable);
static int adc_ioctl(struct adc_dev_s *dev, int cmd, unsigned long arg);
static void adc_read_work(struct adc_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct adc_info_s g_adc_info;
static uint8_t g_vbat_log;

static const struct adc_ops_s g_adcops =
{
    .ao_bind      = adc_bind,
    .ao_reset     = adc_reset,
    .ao_setup     = adc_setup,
    .ao_shutdown  = adc_shutdown,
    .ao_rxint     = adc_rxint,
    .ao_ioctl     = adc_ioctl,
};

static struct adc_dev_s g_adc_chan_dev =
{
    .ad_ops  = &g_adcops,
    .ad_priv = &g_adc_info,
};

static mutex_t g_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int32_t adc_raw_to_pin_mv(struct adc_info_s *ctx, uint32_t value)
{
    float offset;
    float ratio;
    int32_t adc_mv;

    offset = ctx->adc_vol_offset;
    ratio = ctx->adc_vol_ratio;
    if (ratio <= 0.0f)
    {
        ratio = 1000.0f;
    }

    adc_mv = (int32_t)(((float)value - offset) * ratio / 1000.0f);
    if (adc_mv < 0)
    {
        adc_mv = 0;
    }

    return adc_mv;
}

static int adc_convert_raw(ADC_HandleTypeDef *adc_handle, uint8_t channel,
                           int count, uint32_t gap_us, uint32_t *out)
{
    int ret;
    int i;
    int j;
    uint32_t data[ADC_SAMPLE_MAX];
    uint32_t total;
    uint32_t tmp;
    uint32_t used;
    ADC_ChannelConfTypeDef chan_cfg;

    if (out == NULL || channel > ADC_CHAN_7)
    {
        return -EINVAL;
    }

    if (count < 1)
    {
        count = 1;
    }

    if (count > ADC_SAMPLE_MAX)
    {
        count = ADC_SAMPLE_MAX;
    }

    memset(&chan_cfg, 0, sizeof(chan_cfg));
    chan_cfg.pchnl_sel = channel;
    chan_cfg.slot_en = 1;
    chan_cfg.nchnl_sel = 0;
    chan_cfg.Channel = channel;
    chan_cfg.acc_num = 0;

    HAL_ADC_ConfigChannel(adc_handle, &chan_cfg);

    /* CH_SEL=7: ANAU EN_VBAT_MON 接到内部分压点，EN_BG 打开 ANAU bandgap。
     * Prepare 也会置这两位；这里再写一次，避免 TSEN 等关掉 EN_BG。 */
    hwp_hpsys_cfg->ANAU_CR |= (HPSYS_CFG_ANAU_CR_EN_BG |
                               HPSYS_CFG_ANAU_CR_EN_VBAT_MON);

    HAL_ADC_Start(adc_handle);

    total = 0;
    for (i = 0; i < count; i++)
    {
        if (i != 0)
        {
            ADC_SET_UNMUTE(adc_handle);
            HAL_Delay_us(200);
            __HAL_ADC_START_CONV(adc_handle);
        }

        ret = HAL_ADC_PollForConversion(adc_handle, 100);
        if (ret != HAL_OK)
        {
            HAL_ADC_Stop(adc_handle);
            syslog(LOG_ERR, "Polling ADC fail %d\n", ret);
            return -EIO;
        }

        data[i] = (uint32_t)HAL_ADC_GetValue(adc_handle, 0);
        ADC_SET_MUTE(adc_handle);
        total += data[i];

        if (i + 1 < count)
        {
            if (channel == ADC_CHAN_VBAT)
            {
                HAL_Delay_us(1000);
            }
            else if (gap_us > 0)
            {
                HAL_Delay_us(gap_us);
            }
        }
    }

    HAL_ADC_Stop(adc_handle);

    if (count >= 4)
    {
        for (i = 0; i < count - 1; i++)
        {
            for (j = 0; j < count - 1 - i; j++)
            {
                if (data[j] > data[j + 1])
                {
                    tmp = data[j];
                    data[j] = data[j + 1];
                    data[j + 1] = tmp;
                }
            }
        }

        total -= data[0];
        total -= data[count - 1];
        used = (uint32_t)(count - 2);
    }
    else
    {
        used = (uint32_t)count;
    }

    *out = total / used;
    return 0;
}

static int adc_read_channel_mv(uint8_t channel, int count, uint32_t gap_us)
{
    int ret;
    uint32_t raw;
    struct adc_info_s *ctx = &g_adc_info;
    ADC_HandleTypeDef *adc_handle = &ctx->adc_handle;

    if (ctx->initialized != 1)
    {
        return -ENODEV;
    }

    ret = nxmutex_lock(&g_lock);
    if (ret < 0)
    {
        return ret;
    }

    ret = adc_convert_raw(adc_handle, channel, count, gap_us, &raw);
    if (ret == 0)
    {
        ret = (int)adc_raw_to_pin_mv(ctx, raw);
    }

    nxmutex_unlock(&g_lock);
    return ret;
}

static void sf32lb_adc_apply_two_point(struct adc_info_s *priv,
                                       uint16_t vol10, uint16_t vol25,
                                       uint16_t low_mv, uint16_t high_mv)
{
    uint32_t reg_max;
    float gap1;
    float gap2;

    reg_max = GPADC_ADC_RDATA0_SLOT0_RDATA >> GPADC_ADC_RDATA0_SLOT0_RDATA_Pos;
    priv->adc_thd_reg = reg_max > 3 ? reg_max - 3 : reg_max;

    vol10 &= 0x7fff;
    vol25 &= 0x7fff;

    gap1 = vol10 > vol25 ? (float)(vol10 - vol25) : (float)(vol25 - vol10);
    gap2 = low_mv > high_mv ? (float)(low_mv - high_mv)
                            : (float)(high_mv - low_mv);
    if (gap1 < 1.0f)
    {
        return;
    }

    priv->adc_vol_ratio = gap2 * 1000.0f / gap1;
    if (priv->adc_vol_ratio < 1.0f)
    {
        priv->adc_vol_ratio = 1000.0f;
    }

    priv->adc_vol_offset = (float)vol10 -
                           ((float)low_mv * 1000.0f / priv->adc_vol_ratio);

    priv->adc_thd_reg = (uint32_t)(3300.0f * 1000.0f / priv->adc_vol_ratio +
                                   priv->adc_vol_offset);
    if (reg_max > 3 && priv->adc_thd_reg >= (reg_max - 3))
    {
        priv->adc_thd_reg = reg_max - 3;
    }
}

static void sf32lb_adc_apply_vbat_factor(struct adc_info_s *priv,
                                         uint16_t vbat_reg, uint16_t vbat_mv)
{
    float sample_mv;
    float factor;

    priv->adc_vbat_factor = ADC_VBAT_FACTOR_DEFAULT;
    if (vbat_reg == 0 || vbat_mv == 0)
    {
        return;
    }

    sample_mv = ((float)vbat_reg - priv->adc_vol_offset) *
                priv->adc_vol_ratio / 1000.0f;
    if (sample_mv < 200.0f)
    {
        syslog(LOG_WARNING,
               "ADC VBAT factory sample %d mV invalid, use k=%d\n",
               (int)sample_mv, (int)(ADC_VBAT_FACTOR_DEFAULT * 1000.0f));
        return;
    }

    factor = (float)vbat_mv / sample_mv;
    if (factor < ADC_VBAT_FACTOR_MIN || factor > ADC_VBAT_FACTOR_MAX)
    {
        syslog(LOG_WARNING,
               "ADC VBAT factory factor %d/1000 out of range, use k=%d\n",
               (int)(factor * 1000.0f),
               (int)(ADC_VBAT_FACTOR_DEFAULT * 1000.0f));
        return;
    }

    priv->adc_vbat_factor = factor;
}

static void sf32lb_adc_calibrate(struct adc_info_s *priv)
{
    FACTORY_CFG_ADC_T cfg;
    int got;
    int two_ok;

    memset(&cfg, 0, sizeof(cfg));
    priv->adc_vol_ratio = 1000.0f;
    priv->adc_vol_offset = 0.0f;
    priv->adc_vbat_factor = ADC_VBAT_FACTOR_DEFAULT;

    got = BSP_CONFIG_get(FACTORY_CFG_ID_ADC, (uint8_t *)&cfg,
                         (int)sizeof(cfg));
    two_ok = (cfg.vol10 != 0 && cfg.vol25 != 0 &&
              cfg.low_mv != 0 && cfg.high_mv != 0);
    if (!two_ok)
    {
        syslog(LOG_WARNING,
               "ADC eFuse two-point missing (get=%d), use defaults\n", got);
        cfg.vol10 = 1758;
        cfg.vol25 = 3162;
        cfg.low_mv = 1000;
        cfg.high_mv = 2500;
    }

    sf32lb_adc_apply_two_point(priv, cfg.vol10, cfg.vol25,
                               cfg.low_mv, cfg.high_mv);
    sf32lb_adc_apply_vbat_factor(priv, cfg.vbat_reg, cfg.vbat_mv);

#if defined(SF32LB52X)
    if (SF32LB52X_LETTER_SERIES() && cfg.ldovref_flag)
    {
        __HAL_ADC_SET_LDO_REF_SEL(&priv->adc_handle, cfg.ldovref_sel);
    }
#endif

    syslog(LOG_INFO,
           "ADC calib ratio=%d offset=%d vbat_k=%d "
           "(vbat %u mV reg %u get=%d)\n",
           (int)priv->adc_vol_ratio, (int)priv->adc_vol_offset,
           (int)(priv->adc_vbat_factor * 1000.0f + 0.5f),
           (unsigned)cfg.vbat_mv, (unsigned)cfg.vbat_reg, got);
}

static void adc_read_work(struct adc_dev_s *dev)
{
    int pin_mv;
    int report_mv;
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    pin_mv = adc_read_channel_mv(ctx->channel, ADC_QUICK_COUNT,
                                 ADC_QUICK_GAP_US);
    if (pin_mv < 0)
    {
        return;
    }

    report_mv = pin_mv;
    if (ctx->channel == ADC_CHAN_VBAT)
    {
        report_mv = (int)((float)pin_mv * ctx->adc_vbat_factor + 0.5f);
        if (g_vbat_log < 8u)
        {
            syslog(LOG_INFO,
                   "ADC VBAT tap=%d mV pack=%d mV k=%d anau=0x%lx\n",
                   pin_mv, report_mv,
                   (int)(ctx->adc_vbat_factor * 1000.0f + 0.5f),
                   (unsigned long)hwp_hpsys_cfg->ANAU_CR);
            g_vbat_log++;
        }
    }

    if (ctx->cb != NULL && ctx->cb->au_receive != NULL)
    {
        ctx->cb->au_receive(dev, ctx->channel, report_mv);
    }
}

static int adc_bind(struct adc_dev_s *dev,
                    const struct adc_callback_s *callback)
{
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    ctx->cb = callback;

    return OK;
}

static void adc_reset(struct adc_dev_s *dev)
{
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    if (ctx->ref > 0)
    {
        ctx->ref = 0;
    }
}

static int adc_setup(struct adc_dev_s *dev)
{
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    if (ctx->ref > 0)
    {
        ctx->ref++;
        return OK;
    }

    ctx->ref++;

    return OK;
}

static void adc_rxint(struct adc_dev_s *dev, bool enable)
{
    (void)dev;
    (void)enable;
}

static int adc_ioctl(struct adc_dev_s *dev, int cmd, unsigned long arg)
{
    int ret;

    (void)arg;

    switch (cmd)
    {
        case ANIOC_TRIGGER:
            adc_read_work(dev);
            ret = OK;
            break;

        case ANIOC_GET_NCHANNELS:
            ret = 1;
            break;

        case ANIOC_WDOG_UPPER:
            ret = 1;
            break;

        case ANIOC_WDOG_LOWER:
            ret = 1;
            break;

        default:
            syslog(LOG_ERR, "ERROR: Unknown cmd: %d\n", cmd);
            ret = -ENOTTY;
            break;
    }

    return ret;
}

static void adc_shutdown(struct adc_dev_s *dev)
{
    struct adc_info_s *ctx = (struct adc_info_s *)dev->ad_priv;

    if (ctx->ref > 0)
    {
        ctx->ref--;
    }
}

static void sf32lb_adc_default_config(ADC_HandleTypeDef *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->Instance = hwp_gpadc1;
    cfg->Init.atten3 = 0;
    cfg->Init.adc_se = 1;
    cfg->Init.adc_force_on = 0;
    cfg->Init.dma_en = 0;
    cfg->Init.op_mode = 0;
    cfg->Init.en_slot = 0;

#ifndef SF32LB55X
    cfg->Init.data_samp_delay = 2;
#if defined(SF32LB52X)
    cfg->Init.conv_width = 75;
    cfg->Init.sample_width = 71;
#else
    cfg->Init.conv_width = 24;
    cfg->Init.sample_width = 22;
#endif
    cfg->Init.avdd_v18_en = 0;
#else
    cfg->Init.clk_div = 0;
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sf32lb_adc_init(const char *devpath)
{
    int ret = OK;
    struct adc_dev_s *dev;
    struct adc_info_s *ctx;
    ADC_HandleTypeDef *adc_handle;
    ADC_HandleTypeDef loc_ctx;

#ifndef CONFIG_ADC
    syslog(LOG_WARNING, "ADC %s not configured\n", devpath);
    return ret;
#endif

    dev = &g_adc_chan_dev;
    ctx = (struct adc_info_s *)dev->ad_priv;
    adc_handle = (ADC_HandleTypeDef *)&ctx->adc_handle;

    if (ctx->initialized != 1)
    {
        HAL_RCC_EnableModule(RCC_MOD_GPADC);

        sf32lb_adc_default_config(&loc_ctx);
        memcpy(adc_handle, &loc_ctx, sizeof(ADC_HandleTypeDef));

        ctx->channel = ADC_CHAN_VBAT;
        ctx->ref = 0;

        if (HAL_ADC_Init(adc_handle) != HAL_OK)
        {
            syslog(LOG_ERR, "%s init failed\n", devpath);
            return -EIO;
        }

        (void)HAL_ADC_SetFreq(adc_handle, ADC_ATE_FREQ_HZ);
        sf32lb_adc_calibrate(ctx);

        ret = adc_register(devpath, dev);
        if (ret < 0)
        {
            syslog(LOG_ERR, "ADC register failed, devpath=%s, ret=%d\n",
                   devpath, ret);
            return ret;
        }

        ctx->initialized = 1;
    }

    syslog(LOG_INFO, "ADC %s init done, ch=%u ret=%d\n",
           devpath, (unsigned)ctx->channel, ret);

    HAL_Delay_us(300 * 1000);

    return ret;
}
