# ST7789S SPI2 / DMA / Framebuffer 使用与调试指南

> **后续剔除：** Helm One 已改 NV3031A，不再使用 ST7789。本文与
> `st7789s_lcd.c` / `lcd_spi2.c` / `LCD_USING_ST7789S` 一并待删。

本文档归纳 **TK024F3036（ST7789S，320×240）** 在 SF32LB52 板上的显示栈：`lcd_spi2` 专用 SPI 驱动、SPI TX DMA、NuttX `/dev/fb0` 注册与刷新方式，以及 2026-06 调试 ST7789 SPI 显示时的经验与坑点。

**相关源码：**

| 文件 | 说明 |
|------|------|
| `boards/sf32lb52/drivers/lcd/lcd_spi2.c` | 专用 SPI2 驱动（独立于 `sifli_spi.c`） |
| `boards/sf32lb52/drivers/lcd/st7789s_lcd.c` | 板级 LCD 初始化、`lcddev`/`fb` 注册 |
| `boards/sf32lb52/drivers/lcd/tk024f3036_panel.c` | 面板 init 序列、RGB 条测试 |
| `boards/sf32lb52/helmone/nsh/test/test_screen.c` | 全屏刷新压测 `test screen` |
| `boards/sf32lb52/drivers/lcd/Kconfig` | `LCD_ST7789S_SPI2_TX_DMA` 等选项 |

**硬件连接（SPI2，PA37–PA42）：**

| 引脚 | 功能 |
|------|------|
| PA37 | MOSI |
| PA38 | MISO |
| PA39 | SCK |
| PA40 | CS |
| PA41 | 背光 BL |
| PA42 | DC（命令/数据） |

---

## 目录

1. [整体架构](#1-整体架构)
2. [SPI 时钟：48 MHz 上限](#2-spi-时钟48-mhz-上限)
3. [SPI DMA 使用方式](#3-spi-dma-使用方式)
4. [SPI DMA 注意事项与常见坑](#4-spi-dma-注意事项与常见坑)
5. [Framebuffer 模型与使用方式](#5-framebuffer-模型与使用方式)
6. [fb / lcddev 注册注意事项](#6-fb--lcddev-注册注意事项)
7. [Kconfig 与 defconfig](#7-kconfig-与-defconfig)
8. [测试与性能参考](#8-测试与性能参考)
9. [本次 SPI 调试记录（问题 → 原因 → 修复）](#9-本次-spi-调试记录问题--原因--修复)
10. [后续优化方向（可选）](#10-后续优化方向可选)

---

## 1. 整体架构

```
应用 (LVGL / test screen / fb 命令)
    │  open("/dev/fb0") → 写 fbmem → ioctl(FBIO_UPDATE)
    ▼
NuttX video/fb 驱动 (CONFIG_VIDEO_FB)
    │  st7789 framebuffer 适配
    ▼
st7789 LCD 驱动 (CONFIG_LCD_ST7789)
    │  putarea / putrun → SPI sndblock
    ▼
lcd_spi2 (专用 SPI2，不用 sifli_spi 总线)
    │  小数据：HAL_SPI_Transmit 轮询
    │  大块像素：HAL_SPI_Transmit_DMA + 信号量等待
    ▼
ST7789S 面板 (TK024F3036)
```

要点：

- **专用 SPI2**：LCD 走 `lcd_spi2.c`，与通用 `sifli_spi.c`（SPI1 等）隔离，避免 pin/时钟/DMA 冲突。
- **单缓冲 + 手动上屏**：不是 ping-pong 双缓冲；应用写入 framebuffer 内存后，必须 **`ioctl(FBIO_UPDATE)`** 才会推到屏上。
- **DMA 不等于异步上屏**：DMA 期间 CPU 空闲，但**调用线程仍阻塞**等待 DMA 完成（信号量在 DMA ISR 链里 post）。

---

## 2. SPI 时钟：48 MHz 上限

SF32LB52 上 **USART1/2/3、SPI1/2、I2C1/2/3/4** 等工作在 **`clk_peri_hpsys`** 上：

- 源可在 **`clk_hrc48`** 与 **`clk_hxt48`** 间选择（`HPSYS_RCC` 的 `CSR_SEL_PERI`）。
- 两者均为 **48 MHz**，且**独立于系统 HCLK**（例如 HCPU 240 MHz 不影响 SPI 外设时钟）。

因此：

| 项目 | 值 |
|------|-----|
| SPI2 位时钟源 | `clk_peri_hpsys` = **48 MHz** |
| 硬件最高 SCLK | **48 MHz**（`BaudRatePrescaler = 1` → `SPI_CLK_CTRL_CLK_SEL`） |
| 更低频率 | `SCLK = 48 MHz / prescaler`（2→24M，3→16M，4→12M …） |
| **不是** SPI 时钟源 | HCLK（240M）、PCLK1（120M）、PCLK2（3.75M） |

`lcd_spi2_setfrequency()` 必须用 **`LCD_SPI2_PERI_CLK_HZ`（48000000）** 计算分频。若误用 `HAL_RCC_GetHCLKFreq()`，会把 prescaler 算小很多倍，实际总线远低于预期。

**实测 DIAG 示例（修复后）：**

```
DIAG lcd_spi2: freq=48000000 presc=1 hclk=240000000 pclk1=120000000 pclk2=3750000 CBSR=8 burst=8 nbits=16
```

说明：HCLK/PCLK 与 SPI 无关；`freq=48000000 presc=1` 才是有效 SPI 速率。

`CONFIG_LCD_ST7789_FREQUENCY=62500000` 表示「期望 ≥62.5M」，驱动会 clamp 到 **48M**（presc=1），低于 ST7789 面板 62.5M 上限，安全。

---

## 3. SPI DMA 使用方式

### 3.1 开关

Kconfig：`CONFIG_LCD_ST7789S_SPI2_TX_DMA=y`（默认开启）。

### 3.2 何时走 DMA / 何时轮询

| 条件 | 路径 |
|------|------|
| DMA 已 enable，且 `nwords >= LCD_SPI2_DMA_MIN_PIXELS`（512） | `lcd_spi2_tx_dma()` |
| 寄存器/命令/小像素块 | `lcd_spi2_tx_poll()` |
| DMA 启动或等待失败 | 自动 fallback 到 poll |

典型场景：

- **全屏 `FBIO_UPDATE`（76800 像素）** → 单次大块 DMA。
- **`st7789_putarea` 按行刷新**（一行 240~320 像素）→ 小于 512，走 **poll**（避免每行 DMA 初始化开销）。

### 3.3 DMA 通道与 HAL 配置

- 通道定义见 `spi_config.h`：`SPI2_TX_DMA_INSTANCE` / `SPI2_TX_DMA_IRQ` / `SPI2_TX_DMA_REQUEST`（DMAC1 CH8）。
- 方向：`DMA_MEMORY_TO_PERIPH`，16-bit 对齐（RGB565）。
- **`BurstSize = 8`**（`LCD_SPI2_DMA_BURST`）：每次 DMA 服务请求搬 8 个 word，避免 FIFO 饿死；SPI TX FIFO 深 16，burst 不宜超过一半。
- 传输前 **`up_clean_dcache()`**（若 `CONFIG_ARCH_DCACHE`）。

### 3.4 一次 DMA 传输的流程

```
lcd_spi2_do_tx(allow_dma=true)
  └─ lcd_spi2_tx_dma()
       ├─ up_disable_irq(SPI2_IRQn)     // 屏蔽 SPI NVIC，防 TXE 中断风暴
       ├─ for each chunk (max 65535 words):
       │    ├─ lcd_spi2_drain_done()     // 清掉陈旧 sem token
       │    ├─ HAL_DMA_Init()
       │    ├─ HAL_SPI_Transmit_DMA()
       │    └─ lcd_spi2_wait_done()      // nxsem_tickwait，DMA ISR post
       └─ lcd_spi2_spi_irq_restore()     // 清 SPI IT + 恢复 SPI2 NVIC
```

### 3.5 同步模型

- **`sem_done`**：DMA 完成时由 `lcd_spi2_dma_isr` → `lcd_spi2_signal_done()` post。
- 调用方（如 `FBIO_UPDATE` 路径）在 `lcd_spi2_wait_done()` 里阻塞，超时 5 s。
- **CPU 在 DMA 搬运期间空闲**，适合 LVGL 等在等待期间做别的事（若改为异步架构）；当前实现仍是**同步返回**。

### 3.6 调试日志

`lcd_spi2.c` 中 `#define LCD_SPI2_DEBUG 0`，改为 `1` 可打开 `DBG lcd_spi2:` 详细 trace（DMA chunk、ISR 计数、sem post/skip 等）。量产建议保持 `0`。

---

## 4. SPI DMA 注意事项与常见坑

### 4.1 不要用 HCLK 算 SPI 分频

| 误算（HCLK=240M, 目标 62.5M） | 实际 |
|-------------------------------|------|
| presc=4 | 48M/4 = **12 MHz** 总线 |
| 表现 | 全屏 ~170 ms，有效 ~7 Mbit/s |

**修复**：分频源改为 `LCD_SPI2_PERI_CLK_HZ`（48M）→ presc=1 → **48 MHz**。

### 4.2 DMA 期间必须屏蔽 SPI2 NVIC

`HAL_SPI_Transmit_DMA` 会开 `SPI_IT_TXE`，SPI ISR 持续触发（`state=BUSY_TX`），占满 CPU，主线程无法返回，表现为「DMA 卡住」。

**修复**：DMA 传输全程 `up_disable_irq(SPI2_IRQn + 16)`，仅在 DMA 通道 IRQ 里处理完成；结束后 `lcd_spi2_spi_irq_restore()`。

### 4.3 信号量：drain 位置与 double-post

- **错误**：在 `wait_done()` 里先 drain 再 wait → 可能吃掉刚 post 的 token，或 SPI/DMA 双 ISR post 导致状态错乱。
- **正确**：**每个 DMA chunk 开始前** `lcd_spi2_drain_done()`；`wait_done()` 只阻塞等待。
- SPI ISR 在 `dma_inflight==true` 时**忽略**，仅 DMA ISR post `sem_done`。

### 4.4 BurstSize 必须为 8（或 >1）

`BurstSize=0` 时 DMA 每次只搬 1 word，FIFO 频繁饿死，实测比轮询还慢。

寄存器 `CBSR=8` 应与 `LCD_SPI2_DMA_BURST` 一致。

### 4.5 小传输不要用 DMA

`LCD_SPI2_DMA_MIN_PIXELS=512`：单行 partial update 用 DMA 时，每行 `HAL_DMA_Init` + IRQ 开销大于 poll 填 FIFO。

### 4.6 不要在 wait_done 里 usleep 轮询

曾用 `usleep(1000)` 轮询 sem，每 chunk 至少多 1 ms；全屏拆 2 chunk + 多行场景下 FPS 暴跌。

**修复**：`nxsem_tickwait_uninterruptible()` 直接阻塞，微秒级唤醒。

### 4.7 D-Cache

framebuffer 在 cached RAM 时，`FBIO_UPDATE` 前需 `up_clean_dcache(fbmem, len)`，否则 DMA 可能读到旧数据。`tk024f3036_draw_rgb_bars_fb()` 已示范。

### 4.8 全屏数据量与 chunk

76800 像素（16-bit）> `LCD_SPI2_MAX_XFER`（65535），全屏会拆 **2 个 DMA chunk**，有一定额外开销（见 §10）。

---

## 5. Framebuffer 模型与使用方式

### 5.1 模型：单缓冲 + 显式刷新

| 特性 | 说明 |
|------|------|
| 缓冲数量 | **1** 个（`FBIOGET_PLANEINFO` → `fbmem`） |
| 写入 | 直接写 `pinfo.fbmem`（或 mmap 后写） |
| 上屏 | **`ioctl(fd, FBIO_UPDATE, &area)`** |
| 双缓冲/VSYNC | 未实现；不是乒乓模型 |

数据流：

```
CPU 写 fbmem  →  FBIO_UPDATE(area)  →  st7789 putarea  →  SPI/DMA  →  面板 GRAM
```

### 5.2 应用侧标准流程

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/video/fb.h>

int fd = open("/dev/fb0", O_RDWR);

struct fb_videoinfo_s vinfo;
struct fb_planeinfo_s pinfo;
ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)(uintptr_t)&vinfo);
ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)(uintptr_t)&pinfo);

/* RGB565：按 stride 逐行写 pinfo.fbmem */
uint16_t *row = (uint16_t *)((uint8_t *)pinfo.fbmem + y * pinfo.stride);
row[x] = 0xF800;

#ifdef CONFIG_ARCH_DCACHE
up_clean_dcache((uintptr_t)pinfo.fbmem,
                (uintptr_t)pinfo.fbmem + pinfo.stride * vinfo.yres);
#endif

struct fb_area_s area = { .x = 0, .y = 0, .w = vinfo.xres, .h = vinfo.yres };
ioctl(fd, FBIO_UPDATE, (unsigned long)(uintptr_t)&area);

close(fd);
```

### 5.3 依赖配置

必须同时开启：

- `CONFIG_VIDEO_FB=y` — 注册 `/dev/fb0`
- `CONFIG_FB_UPDATE=y` — 支持 `FBIO_UPDATE` ioctl
- `CONFIG_LCD_ST7789=y` + `CONFIG_LCD_USING_ST7789S=y`

未开 `CONFIG_FB_UPDATE` 时，写 fbmem **不会**显示到屏上。

### 5.4 NSH 内置测试

| 命令 | 作用 |
|------|------|
| `fb` | NuttX 自带 framebuffer 演示（矩形动画） |
| `test screen [秒]` | 全屏刷新压测，打印 fps / min-max / throughput |

`test screen` 的 **frame min/max** 只统计单次 `FBIO_UPDATE` 耗时；**frame avg** = 总时间/帧数，含 CPU 填 fbmem 时间，因此 avg 常大于 min。

---

## 6. fb / lcddev 注册注意事项

### 6.1 初始化顺序（`st7789s_lcd.c`）

```
board_lcd_initialize()
  ├─ BSP_LCD_PowerUp()
  ├─ lcd_spi2_dev_initialize()      // SPI2 + 可选 DMA
  └─ task_create("lcd_init", ...)   // 异步，避免阻塞 board init

lcd_init 线程:
  ├─ tk024f3036_panel_init(spi)
  ├─ st7789_lcdinitialize(spi)    // 得到 g_lcd
  ├─ tk024f3036_panel_apply_video_mode(spi)
  ├─ st7789s_register_devices()
  │    ├─ lcddev_register(0)      // /dev/lcd0（CONFIG_LCD_DEV）
  │    └─ fb_register(0, 0)       // /dev/fb0（CONFIG_VIDEO_FB）
  ├─ RGB 条测试 (FBIO_UPDATE)
  └─ BSP_LCD_BL_Set(1)
```

**要点：**

1. **`board_lcd_getdev()`** 必须能返回已初始化的 `g_lcd`，供 NuttX fb/lcd 框架绑定；在 `g_lcd==NULL` 时会 lazy init。
2. **`fb_register` 依赖 lcd 已就绪**；过早调用会 `-ENOENT`/`-ENODEV`。
3. 注册使用 **最多 30 次、间隔 100 ms 的重试**，直到 `OK` 或 `-EEXIST`。
4. LCD 初始化放在 **独立任务** `lcd_init`，避免 `fb_register` 首帧全屏 DMA 阻塞 `board_lcd_initialize` 过久。

### 6.2 注册返回值

| 返回值 | 处理 |
|--------|------|
| `OK` | 成功 |
| `-EEXIST` | 已注册，视为成功 |
| `-ENOENT` / `-ENODEV` | 依赖未就绪，继续 retry |
| 其他 | 打日志并停止 retry |

### 6.3 fb 与 LVGL

LVGL NuttX 端口通常：

1. `open("/dev/fb0")` + `FBIOGET_*`
2. 用 `fbmem` 作为 draw buffer
3. 每帧或脏区 `FBIO_UPDATE`

与本文档单缓冲模型一致；要高帧率需应用层减少无效全屏 refresh，或后续做双缓冲流水线（§10）。

---

## 7. Kconfig 与 defconfig

`my_vendor/configs/nsh/defconfig` 中与 ST7789 fb 相关片段：

```
CONFIG_LCD_USING_ST7789S=y
CONFIG_LCD_ST7789S_SPI2_TX_DMA=y
CONFIG_LCD_ST7789=y
CONFIG_LCD_ST7789_XRES=240
CONFIG_LCD_ST7789_YRES=320
CONFIG_LCD_ST7789_FREQUENCY=62500000
CONFIG_LCD_ST7789_SPIMODE=3
CONFIG_VIDEO_FB=y
CONFIG_FB_UPDATE=y
```

说明：`XRES/YRES` 与面板物理 320×240 的宽高对应关系以 NuttX st7789 驱动约定为准（当前配置下 fb 报告 **320×240**）。

软 SPI 调试（仅 bring-up）：

```
CONFIG_LCD_ST7789S_SOFTSPI=y   # GPIO bit-bang，不走硬件 SPI2/DMA
```

---

## 8. 测试与性能参考

### 8.1 修复前（错误 HCLK 分频 + DMA 已开）

```
DIAG: freq=60000000 presc=4 hclk=240000000 CBSR=8 burst=8 nbits=16   // freq 为误算值
test screen: fps ~5.33, frame min ~171 ms
```

### 8.2 修复后（48 MHz 真实满速）

```
DIAG: freq=48000000 presc=1 hclk=240000000 pclk1=120000000 pclk2=3750000 CBSR=8 burst=8 nbits=16
RGB fb FBIO_UPDATE 320x240  ~44 ms（boot 日志段）
test screen: frames=84 / 5s, fps=16.67, frame min ~43 ms, frame avg ~60 ms
```

解读：

| 指标 | 值 | 含义 |
|------|-----|------|
| frame min ~43 ms | ~23 fps | 纯 `FBIO_UPDATE`/SPI 上屏 |
| frame avg ~60 ms | ~16.7 fps | 含每帧 CPU 填充 153600 字节 |
| 理论极限 | ~25.6 ms | 48M × 16bit 连续传 76800 像素 |
| 当前效率 | ~60% | 余量来自 chunk 拆分、setarea 命令、DMA 重初始化等 |

若 48 MHz 走线过长导致花屏，可将 `CONFIG_LCD_ST7789_FREQUENCY` 设为 `24000000`（实际 24M，presc=2）。

---

## 9. 本次 SPI 调试记录（问题 → 原因 → 修复）

| # | 现象 | 根因 | 修复 |
|---|------|------|------|
| 1 | `fb_register` 首帧全屏 DMA 卡死，日志停在 `spi_isr #N` | `HAL_SPI_Transmit_DMA` 开启 SPI TXE 中断，SPI ISR 风暴占满 CPU | DMA 期间 `up_disable_irq(SPI2_IRQn)`，仅 DMA CH8 IRQ 完成同步 |
| 2 | DMA 完成但线程仍超时 | sem 在 `wait_done` 里被 drain，或 SPI/DMA 双 ISR post | chunk 前 `drain_done`；SPI ISR 在 `dma_inflight` 时忽略；仅 DMA 路径 post |
| 3 | 开启 DMA 后比轮询更慢 | `BurstSize=0`，DMA 每次 1 word，FIFO 饿死 | `LCD_SPI2_DMA_BURST=8`，确认 `CBSR=8` |
| 4 | 部分刷新极慢 | `wait_done` 内 `usleep(1000)` | 改回 `nxsem_tickwait_uninterruptible` |
| 5 | 按行 putarea 慢 | `DMA_MIN_PIXELS=32`，每行都 DMA init | 提高到 **512**，小行用 poll |
| 6 | 全屏 ~170 ms，有效 ~7 Mbit/s，burst 无效 | **分频按 HCLK(240M) 算，实际 SPI 源为 clk_peri 48M** | `clk_src = LCD_SPI2_PERI_CLK_HZ`，presc=1 → **48M** |
| 7 | 误以为 PCLK 是 SPI 时钟 | SPI/USART/I2C 用独立 `clk_peri_hpsys` | 文档化；DIAG 打印 hclk/pclk1/pclk2 便于对照 |

**关键结论：**

1. DMA 是否生效与 **fb 是否手动 `FBIO_UPDATE`** 无关——fb 始终是「写内存 + 手动刷新」；DMA 只优化刷新时的 SPI 搬运。
2. 性能瓶颈最终在 **SPI 位时钟**，不在 DMA 仲裁；时钟源搞错时，调 DMA 参数几乎无收益。
3. SF32LB52 上该路 SPI **最高 48 MHz**，无法通过提高 HCLK 突破。

---

## 10. 后续优化方向（可选）

当前 **~43 ms/帧** 已满足基本 UI；若需逼近 ~25 ms 理论值，可考虑：

1. **减少每 chunk 的 `HAL_DMA_DeInit/Init`**（初始化一次，多次复用）。
2. **提高单次 DMA 长度上限**或合并 setarea，减少全屏 2 chunk 切换。
3. **应用层双缓冲**：CPU 填 back buffer 的同时 DMA 刷 front（需改驱动/应用协作，非当前 fb 模型自带）。
4. **硬件升级路径**：并行 RGB、QSPI、或 SoC LCDC——均超出纯 SPI2 48M 上限。

---

## 修订记录

| 日期 | 说明 |
|------|------|
| 2026-06-11 | 初版：归纳 ST7789S TK024F3036 SPI2 DMA、fb 使用与调试过程 |
