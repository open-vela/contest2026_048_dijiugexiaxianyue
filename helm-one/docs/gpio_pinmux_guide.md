# SF32LB52 引脚交换矩阵与驱动使用说明（my_vendor）

本文分三大节：**第一节**说明芯片交换矩阵与 HAL 路由；**第二节**说明板级 Pinmux 文件 `bsp_pinmux.c` 的职责、结构与默认引脚；**第三节**以串口、I2C 为例说明 NuttX 驱动注册与使用测试。

**芯片**：SF32LB52  
**默认板级**：`boards/sf32lb52/helmone`  
**关联文档**：[test_app_guide.md](test_app_guide.md)、[nuttx_boot_flow.md](nuttx_boot_flow.md)

---

## 目录

### 第一节 交换矩阵

1. [能力表：`bf0_pin_const.c`](#11-能力表bf0_pin_constc)
2. [HAL 路由：`bf0_hal_pinmux.c`](#12-hal-路由bf0_hal_pinmuxc)

### 第二节 板级 Pinmux：`bsp_pinmux.c`

1. [职责与调用链](#21-职责与调用链)
2. [函数结构](#22-函数结构)
3. [默认引脚分配表](#23-默认引脚分配表)
4. [条件编译与注意事项](#24-条件编译与注意事项)
5. [与能力表的分工](#25-与能力表的分工)

### 第三节 驱动注册与使用

1. [驱动注册示例：`sifli_uart.c`](#31-驱动注册示例sifli_uartc)
2. [串口驱动](#32-串口驱动)
3. [I2C 驱动](#33-i2c-驱动)

---

# 第一节 交换矩阵

## 1.1 能力表：`bf0_pin_const.c`

**路径**：`vendor/HelmOne/chips/drivers/cmsis/sf32lb52x/bf0_pin_const.c`  
**头文件**：同目录 `bf0_pin_const.h`（`PAD_PAxx`、`USART2_TXD`、`PA20_I2C_UART` 等枚举）

### 1.1.1 这张表是什么

`pin_pad_func_hcpu[][]` 描述 **HCPU 每个物理脚** 在 Pinmux **16 个槽位**上可选的功能列表；`pin_pad_func_lcpu[][]` 对应 LCPU（PB 脚）。

手册里常只写「支持 UART/I2C」，在源码里对应 **`PAxx_I2C_UART`**（多在某一固定槽位列）——表示该脚可进入 **UART/I2C 交换网络**，**尚未指定是 USART1 还是 I2C2**。

### 1.1.2 怎么读一行

以 PA20 为例（`pin_pad_func_hcpu` 中 `GPIO_A20` 那一行）：

```c
{GPIO_A20,  0,  0,  0,  PA20_I2C_UART,  PA20_TIM, ...},
```

| 含义 | 说明 |
|------|------|
| 行 | 一个 `PAD_PAx`（行序与 `PAD_PA00` 起递增对应，前面还有 SA 等专用脚行） |
| 列 0～15 | 该脚 FSEL 可选的 `pin_function` |
| `PA20_I2C_UART` | 可参与 HCPU 的 USART1～3 / I2C1～4 路由 |
| `GPIO_A20` | 也可配成普通 GPIO |

**LB52 HCPU**：`GPIO_A0`～`GPIO_A44`（**PA00～PA44**）均有 `PAxx_I2C_UART`，均可作为 USART2 的 TX/RX 候选（由软件 + PINR 选择）。

**不能当 HCPU USART2 用**：SA00～SA12（PSRAM/MPI）；PB00～PB03（`PBxx_UART` → USART4/5）。

### 1.1.3 快速查找命令

```bash
grep -n "pin_pad_func_hcpu" vendor/HelmOne/chips/drivers/cmsis/sf32lb52x/bf0_pin_const.c
grep "PA30_I2C_UART" vendor/HelmOne/chips/drivers/cmsis/sf32lb52x/bf0_pin_const.c
```

---

## 1.2 HAL 路由：`bf0_hal_pinmux.c`

能力表只回答「这根脚 **能不能** 接某类外设」；具体 **USART2_TXD**、**I2C1_SCL** 等实例信号由 HAL 写 **`hpsys_cfg.h`** 中的 `USARTx_PINR` / `I2Cx_PINR` 完成选脚。

- **路径**：`vendor/HelmOne/chips/drivers/hal/bf0_hal_pinmux.c`
- **关键实现**（`SF32LB52X` 段）：`HAL_PIN_SetUartFunc`、`HAL_PIN_SetI2cFunc` 等
- **对外接口**：`HAL_PIN_Set(PAD_PA20, USART2_RXD, PIN_PULLUP, 1);`

板级代码通过 `HAL_PIN_Set()` 调用上述逻辑，**不直接操作** `PINR` 寄存器。

---

# 第二节 板级 Pinmux：`bsp_pinmux.c`

**路径**：`vendor/HelmOne/boards/sf32lb52/helmone/bsp/bsp_pinmux.c`

本节说明 my_vendor 板子上 **各外设实际接在哪根脚**、由哪些函数配置，以及改脚时应注意什么。

## 2.1 职责与调用链

`bsp_pinmux.c` 把 **原理图上的实际接线** 写成一系列 `HAL_PIN_Set()` / `HAL_PIN_Set_Analog()` 调用：为每个 PAD 选定外设信号（含实例号，如 `USART2_TXD`）、上下拉模式。

| 注意 | 说明 |
|------|------|
| 只改能力表不够 | 须在 `bsp_pinmux.c` 里写出板级接线 |
| 只改本文件不够 | 须在 Kconfig / `defconfig` 中打开对应外设，驱动才会注册 `/dev/*` 节点 |

**调用时机**（启动早期，外设驱动初始化之前）：

```
HAL_MspInit()
  └── BSP_IO_Init()          /* boards/.../bsp/bsp_init.c */
        └── BSP_PIN_Init()   /* bsp_pinmux.c */
              ├── BSP_PIN_Common()   /* PSRAM/SD/MPI/UART/SPI/USB/GPIO… */
              └── BSP_PIN_LCD()      /* LCD 接口 + 可选触摸 */
```

`BSP_PIN_Touch()` 在 `BSP_PIN_LCD()` 内、开启 `BSP_USING_TOUCHD` 时调用；也可由 `bsp_lcd_tp.c` 等路径单独触发。

## 2.2 函数结构

| 函数 | 可见性 | 作用 |
|------|--------|------|
| `board_pinmux_psram_func0()` | `static` | APS 128Mb PSRAM（MPI1）引脚 |
| `board_pinmux_psram_func1_2_4()` | `static` | APS 64/32Mb、Winbond PSRAM 引脚 |
| `board_pinmux_psram_func3()` | `static` | APS 16Mb PSRAM 引脚 |
| `board_pinmux_mpi1_none()` | `static` | 无 PSRAM 时 SA00～SA12 设为模拟脚 |
| `BSP_PIN_Common()` | `static` | HCPU 通用外设：PSRAM、SDIO/MPI2、UART、SPI、USB、GPIO 等 |
| `BSP_PIN_Touch()` | 全局 | 触摸复位/中断 GPIO + I2C1 SCL/SDA |
| `BSP_PIN_LCD()` | 全局 | LCD 背光 PWM、LCDC 总线（QSPI / DBI / SPI）、LCD 电源 GPIO |
| `BSP_PIN_Init()` | 全局 | 入口：`BSP_PIN_Common()` + `BSP_PIN_LCD()` |

PSRAM 相关 `static` 函数仅在 `BSP_USING_PSRAM1` 时编译；`BSP_PIN_Common()` 主体在 `SOC_BF0_HCPU` 下。

## 2.3 默认引脚分配表

以下为 **my_vendor 默认板** 当前源码中的引脚（改板时请同步更新本表与 `bsp_pinmux.c`）。

### PSRAM / MPI1（SA 脚，`BSP_USING_PSRAM1`）

按芯片 `IDR` 中的 PID 自动选择 `board_pinmux_psram_func*()`；SA00～SA12 专用于 PSRAM，**不可**作普通 UART/I2C。

### 存储与总线（`BSP_PIN_Common`）

| 外设 | 条件宏 | 引脚 | 信号 |
|------|--------|------|------|
| SDIO | `BSP_USING_SDIO` | PA12～PA17 | SD1_DIO2/3/CLK/CMD/DIO0/1 |
| MPI2（外 Flash） | `BSP_ENABLE_MPI2` | PA12～PA17 | MPI2_CLK/CS/DIO0～3 |
| SPI1（TF 卡） | 始终（HCPU） | PA24/25/28/29 | SPI1_DIO/DI/CLK/CS |

> SDIO 与 MPI2 **共用 PA12～PA17**，二者不可同时开启（源码 `#error` 保护）。

### 通信接口

| 外设 | 引脚 | 信号 | NuttX 节点（第三节） |
|------|------|------|----------------------|
| UART1（debug/console） | PA18 RX, PA19 TX | USART1 | `/dev/console` |
| UART2（log） | PA20 RX, PA27 TX | USART2 | `/dev/ttyS0` |
| I2C1（触摸） | PA30 SCL, PA33 SDA | I2C1 | `/dev/i2c0`（在 `BSP_PIN_Touch`） |
| USB Device | PA35, PA36 | USB_DP/DM（模拟脚） | CDC 等 |

`BSP_PIN_Common()` 中 I2C1 的 `HAL_PIN_Set` **已注释**；触摸 I2C 仅在 `BSP_PIN_Touch()` 里配置。

**串口示例（节选）：**

```c
// UART1 - debug / console
HAL_PIN_Set(PAD_PA18, USART1_RXD, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA19, USART1_TXD, PIN_PULLUP, 1);

// UART2 - log → NuttX /dev/ttyS0
HAL_PIN_Set(PAD_PA20, USART2_RXD, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA27, USART2_TXD, PIN_PULLUP, 1);
```

**触摸 I2C（`BSP_PIN_Touch`）：**

```c
HAL_PIN_Set(PAD_PA09, GPIO_A9,  PIN_NOPULL, 1);    // CTP_RESET
HAL_PIN_Set(PAD_PA31, GPIO_A31, PIN_NOPULL, 1);    // CTP_INT
HAL_PIN_Set(PAD_PA30, I2C1_SCL, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA33, I2C1_SDA, PIN_PULLUP, 1);
```

TX/RX 在芯片允许范围内可对调，但必须与硬件「MCU_TX 接模块_RX」一致；PA18/PA19 亦可作 SWD，作调试口时通常被 UART1 占用。

### 显示（`BSP_PIN_LCD`，按 `BSP_LCDC_USING_*` 选一）

| 模式 | 宏 | 主要引脚 |
|------|-----|----------|
| QADSPI | `BSP_LCDC_USING_QADSPI` | PA01 背光 PWM；PA02～PA08 LCDC1_SPI_*；PA00/PA10 LCD 复位/电源 |
| DBI 8080 | `BSP_LCDC_USING_DBI` | PA01 背光；PA02～PA08 + PA37/39～43 LCDC1_8080_* |
| SPI DCX | `BSP_LCDC_USING_SPI_DCX_1DATA` | PA02～PA06 LCDC1_SPI_* |

开启 `BSP_USING_TOUCHD` 时，QSPI 模式下 `BSP_PIN_LCD()` 末尾调用 `BSP_PIN_Touch()`。

### 普通 GPIO / 其它

| 用途 | 引脚 | 说明 |
|------|------|------|
| LCD 复位 | PA00 | `GPIO_A0`，Common 与 LCD 段均有配置 |
| 音频 PA 控制 | PA10 | `GPIO_A10` |
| 按键 Key2 | PA11 | `GPIO_A11`，下拉 |
| 电源键 Key1 | PA34 | 注释掉，保留下拉以免影响 UART 烧录 |
| LED / RGB | PA32 | `GPTIM2_CH1` 与 `GPIO_A32`（同脚复用配置） |
| 通用 GPIO | PA21, PA26, PA38 | 可自由使用 |
| VBUS 检测 | PA44 | `GPIO_A44` |
| 32K 晶振 | PA22/PA23 | 注释保留，勿随意改 |

## 2.4 条件编译与注意事项

### NAND Boot 下勿重配 PSRAM 脚

`CONFIG_BSP_USING_SPI_NAND` 时，应用 XIP 运行于 PSRAM，二级 Bootloader 已配好 MPI1 脚。**再次** `HAL_PIN_Set()` 会毛刺指令取指路径并随机死机，故 `BSP_PIN_Common()` 跳过 PSRAM pinmux，保持 Bootloader 配置不变。

### 外设互斥

- `BSP_USING_SDIO` 与 `BSP_ENABLE_MPI2` 不能同时定义（除 `BSP_USING_BOARD_SF32LB52_LCD_52J_SD` 特例外）。
- 改脚前在 `bf0_pin_const.c` 确认目标 PAD 支持对应 `pin_function`。

### 改脚最小步骤

1. 查 `bf0_pin_const.c` 确认能力（如 `PAxx_I2C_UART`）。
2. 修改 `bsp_pinmux.c` 中对应 `HAL_PIN_Set()`。
3. 在 `configs/nsh/defconfig` 或 `chips/sf32lb52/Kconfig` 打开外设（如 `CONFIG_BSP_USING_UART2`）。

## 2.5 与能力表的分工

| 文件 | 回答的问题 |
|------|------------|
| `bf0_pin_const.c` | 这根脚 **能不能** 接 UART/I2C/SPI… |
| `bf0_hal_pinmux.c` | `HAL_PIN_Set` 如何把信号路由到 **具体外设实例** |
| `bsp_pinmux.c` | 这块板子 **实际接在哪**、用哪个 USART/I2C 实例 |

---

# 第三节 驱动注册与使用

Pinmux 在 `BSP_PIN_Init()` 生效后，NuttX 驱动层再将外设注册为 `/dev/*` 字符设备。本节说明注册机制，并以串口、I2C 为例演示测试。

## 3.1 驱动注册示例：`sifli_uart.c`

**路径**：`vendor/HelmOne/chips/sf32lb52/sifli_uart.c`  
**配置**：`chips/boards/include/config/sf32lb52x/uart_config.h`、`include/sf32lb_serial.h`

### 3.1.1 Kconfig 决定编译进哪些 UART

`configs/nsh/defconfig` 示例：

```
CONFIG_BSP_USING_UART=y
CONFIG_BSP_USING_UART1=y
CONFIG_BSP_USING_UART2=y
```

`uart_config.h` 映射为硬件实例：

```c
#define UART1_CONFIG  { .name = "uart1", .Instance = USART1, .irq_type = USART1_IRQn, }
#define UART2_CONFIG  { .name = "uart2", .Instance = USART2, .irq_type = USART2_IRQn, }
```

`uart_obj[]` / `uart_config[]` 仅包含已 `CONFIG_BSP_USING_UARTx` 的项。

### 3.1.2 注册入口：`arm_serialinit()`

启动阶段由架构/板级初始化调用（与 `arm_earlyserialinit` 先后配合）：

```c
void arm_serialinit(void)
{
    ...
    sifli_usart_init();

    /* ① 固定：USART1 → console */
#ifdef CONSOLE_UART
    uart_register("/dev/console", &(uart_obj[CONSOLE_UART-1].serial));
#endif

    /* ② 其余 UART → /dev/ttyS0, ttyS1, … */
    strcpy(devname, "/dev/ttySx");
    for (i = 1; i <= obj_num; i++) {
#ifdef CONSOLE_UART
        if (i == CONSOLE_UART)
            continue;          /* 跳过已注册为 console 的那路 */
#endif
        devname[9] = '0' + minor++;
        uart_register(devname, &(uart_obj[i-1].serial));
    }
}
```

**`CONSOLE_UART`** 在 `sf32lb_serial.h` 中 **固定为 1**（USART1），不是「第一个启用的 UART」自动当 console。

### 3.1.3 当前 my_vendor 设备名对照

| 硬件 | `uart_obj` 下标 | NuttX 设备 |
|------|-----------------|------------|
| USART1 | 0 | `/dev/console` |
| USART2 | 1 | `/dev/ttyS0` |

**没有** `/dev/uart2`；**`ttyS0` = 串口 2**。

### 3.1.4 `uart_register` 做了什么 / 没做什么

`nuttx/drivers/serial/serial.c` 中 `uart_register()`：

- `register_driver()` 挂到 `/dev`
- 初始化锁、信号量、环形缓冲（每路约 `2 × CONFIG_UART_BUFSZ` RAM）

**此时一般不调用** `HAL_UART_Init()`。

### 3.1.5 何时真正启动硬件

| 阶段 | 行为 |
|------|------|
| `arm_earlyserialinit()` | 本板 **不重配** USART1，沿用 Bootloader 已工作的 console（见 `sifli_lowput.c`） |
| `BSP_PIN_Init()` | **Pinmux 已生效**（脚已是 UART 复用） |
| 首次 `open("/dev/ttyS0")` | `sifli_setup()` → `HAL_UART_Init()`；`sifli_attach()` → IRQ、RX DMA |

因此：**注册 ≠ 外设立刻满载运行**；未 `open` 的 `ttyS0` 通常尚未 `HAL_UART_Init`，但仍占用 Pinmux 与驱动缓冲 RAM。

### 3.1.6 与其它驱动的对比（便于记忆）

| 驱动 | 注册函数 | 典型注册位置 | 设备节点 |
|------|----------|--------------|----------|
| UART | `uart_register()` | `arm_serialinit()` | `/dev/console`, `/dev/ttyS0` |
| I2C | `i2c_register()` | 板级 `sifli_ap.c` bringup | `/dev/i2c0` |
| GPIO | `gpio_register()` | `sifli_gpio_initialize()` | `/dev/gpio0` … |

---

## 3.2 串口驱动

### 3.2.1 前提

- `CONFIG_BSP_USING_UART1/2=y`
- `bsp_pinmux.c` 已 `HAL_PIN_Set(..., USARTx_TXD/RXD, ...)`
- `ls /dev` 能看到 `console`、`ttyS0`

### 3.2.2 串口驱动的 NSH 测试

**看设备：**

```bash
nsh> ls /dev
nsh> ls -l /dev/console
nsh> ls -l /dev/ttyS0
```

**说明：**

- NSH 本身占用 **`/dev/console`**（USART1，PA18/19），波特率与 `CONFIG_UART_BAUD` 一致（常用 **1000000**）。
- **`/dev/ttyS0` = USART2**（PA20/27），不要用 `ttyS1` 找「串口 2」。

**向 UART2 发数据（需 PA20/27 接 USB 转串口或逻辑分析仪）：**

```bash
nsh> echo hello >/dev/ttyS0
```

**从 UART2 读（会占用终端，测试完 Ctrl+C）：**

```bash
nsh> cat /dev/ttyS0
```

**注意：** 不要 `cat /dev/console` 与 NSH 抢同一口；调试第二路用 `ttyS0`。

### 3.2.3 串口驱动的代码测试

应用里打开 **USART2** 示例（`test_app` 等 builtin 中可加）：

```c
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

void test_uart2(void)
{
  int fd;
  const char msg[] = "uart2 test\r\n";
  char buf[32];
  ssize_t n;

  fd = open("/dev/ttyS0", O_RDWR);
  if (fd < 0) {
      printf("open ttyS0 failed\n");
      return;
  }

  write(fd, msg, strlen(msg));

  /* 若对端有回环或设备会应答，可 read；否则可能阻塞 */
  n = read(fd, buf, sizeof(buf) - 1);
  if (n > 0) {
      buf[n] = '\0';
      printf("recv: %s\n", buf);
  }

  close(fd);
}
```

**`printf` / NSH 日志** 走 **`/dev/console`（USART1）**，不会自动写到 `ttyS0`。

改波特率可用 `termios`（`CONFIG_SERIAL_TERMIOS`）或保持与 `CONFIG_UART_BAUD` 一致。

---

## 3.3 I2C 驱动

### 3.3.1 前提

- `CONFIG_I2C`、`CONFIG_I2C_DRIVER`、`CONFIG_BSP_USING_I2C1` 等
- Pinmux：`I2C1_SCL/SDA`（默认 PA30/PA33，见第二节 `BSP_PIN_Touch`）
- **注册在板级 bringup**，不是 `arm_serialinit` 里

**路径**：`vendor/HelmOne/boards/sf32lb52/helmone/bsp/sifli_ap.c`

```c
i2c0 = sifli_i2cbus_initialize(0);   /* port 0 → 硬件 I2C1 */
i2c_register(i2c0, 0);               /* → /dev/i2c0 */
```

**驱动实现**：`vendor/HelmOne/chips/sf32lb52/sifli_i2c.c`  
**配置**：`chips/boards/include/config/sf32lb52x/i2c_config.h`

与 UART 差异：`sifli_i2cbus_initialize()` 时会 **`sifli_i2c_init()`**，总线初始化通常早于应用 `open`。

### 3.3.2 I2C 驱动的 NSH 测试

需 `CONFIG_SYSTEM_I2CTOOL=y`（defconfig 已开），NSH 命令 **`i2c`**。  
**Bus 0** = `/dev/i2c0` = 硬件 **I2C1**（PA30 SCL / PA33 SDA）。

建议按下面顺序做一遍，确认总线、扫描、读写都正常。

#### 步骤 1：确认总线已注册

```bash
nsh> i2c bus
```

预期（仅开 I2C1 时）：

```
 BUS   EXISTS?
Bus 0: YES
Bus 1: NO
...
```

#### 步骤 2：全地址扫描（看总线上有哪些设备）

**7 位地址全范围 `0x00`～`0x7f`：**

```bash
nsh> i2c dev -b 0 0x00 0x7f
```

默认总线已是 0，也可省略 `-b 0`：

```bash
nsh> i2c dev 0x00 0x7f
```

**如何看输出：**

- 打印 16×8 网格；格子里的 **十六进制数** = 有 ACK 的从机 **7 位地址**。
- **`--`** = 该地址无应答。
- 开头可能提示：部分器件对「读 1 字节」探测不应答，需用 **`-z`**（见步骤 2b）。

**本板触摸 FT6146**（`boards/sf32lb52/drivers/input/ft6146.c`）地址为 **`0x38`**，触摸已接且上电时，网格里通常能看到 **`38`**（在 `30:` 行第 9 列附近）。

#### 步骤 2b：全扫描（0 字节写，可选）

若步骤 2 几乎全是 `--`，再试：

```bash
nsh> i2c dev -b 0 -z 0x00 0x7f
```

`-z` 用 **0 字节写** 探测，部分传感器只在这种扫描下才会出现。

#### 步骤 3：读寄存器（`i2c get`）

在步骤 2 里记下地址后，对该从机读寄存器。语法：

```text
i2c get -b <总线> -a <7位地址> -r <寄存器> -l <字节数>
```

**通用示例**（地址 `0x50`，寄存器 `0x00`，读 1 字节）：

```bash
nsh> i2c get -b 0 -a 0x50 -r 0x00 -l 1
```

**本板 FT6146 示例**（读 ID 高字节寄存器 `0xa3`，1 字节）：

```bash
nsh> i2c get -b 0 -a 0x38 -r 0xa3 -l 1
```

成功会打印读到的十六进制数据；失败多为无设备、地址错或触摸未上电。

#### 步骤 4：写寄存器（`i2c set`）

```text
i2c set -b <总线> -a <7位地址> -r <寄存器> <写入值>
```

**通用示例**（向 `0x50` 的寄存器 `0x00` 写 `0x12`）：

```bash
nsh> i2c set -b 0 -a 0x50 -r 0x00 0x12
```

写后可用 `i2c get` 同寄存器读回核对（设备需支持读回）。

#### 常用命令速查

| 命令 | 作用 |
|------|------|
| `i2c bus` | 哪些 `/dev/i2cN` 已存在 |
| `i2c dev -b 0 0x00 0x7f` | **Bus 0 全扫描** |
| `i2c dev -b 0 -z 0x00 0x7f` | 全扫描（0 字节写） |
| `i2c get -b 0 -a <addr> -r <reg> -l <n>` | 读寄存器 |
| `i2c set -b 0 -a <addr> -r <reg> <val>` | 写寄存器 |
| `i2c help` | 子命令与选项说明 |

**注意：** 扫描只能发现 **地址**，不能识别芯片型号；触摸驱动运行中也可扫描，但频繁全扫描可能影响触摸，测试完即可。

**窄范围扫描（可选，更快）：**

```bash
nsh> i2c dev -b 0 0x03 0x77
```

### 3.3.3 I2C 驱动的代码测试

**方式 A：经 `/dev/i2c0`（与 i2ctool 相同底层）**

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/i2c/i2c_master.h>
#include <stdio.h>

/* 读：需按 NuttX i2c_msg_s 组 ioctl，略繁；推荐先用 NSH i2c 验证总线 */
```

**方式 B：板级已 `i2c_register` 后，其它驱动用法（与 `sifli_ap.c` 一致）**

在已实现 bringup 的系统里，触摸等驱动在注册后持有 `struct i2c_master_s *`，应用层更常见是：

1. 自己 `sifli_i2cbus_initialize(0)` + `i2c_register`（不推荐重复注册），或  
2. 使用标准 **`I2C_DRIVER`** 字符设备接口 / 已有传感器驱动。

**最小验证建议**：先用 **`i2c bus` / `i2c dev`** 确认 `/dev/i2c0` 可用，再在应用里封装 `ioctl` 或链接已有 I2C 从设备驱动。

**打开设备节点检查：**

```bash
nsh> ls -l /dev/i2c0
```

---

## 附录：改脚 / 增路快速对照

| 目标 | 改 `bf0_pin_const` | 改 `bsp_pinmux.c` | Kconfig | 设备节点 |
|------|---------------------|-------------------|---------|----------|
| 换 UART2 脚 | 查 `PAxx_I2C_UART` | `USART2_TXD/RXD` | `CONFIG_BSP_USING_UART2` | `/dev/ttyS0` |
| 换 I2C1 脚 | 同上 | `I2C1_SCL/SDA` | `CONFIG_BSP_USING_I2C1`、`BSP_I2C1_*_PIN` | `/dev/i2c0` |
| 关掉 UART2 省电 | — | 去掉 USART2 `HAL_PIN_Set` | 关闭 `CONFIG_BSP_USING_UART2` | 无 `ttyS0` |

---

## 相关文件索引

| 主题 | 文件 |
|------|------|
| 能力表 | `chips/drivers/cmsis/sf32lb52x/bf0_pin_const.c` |
| HAL 路由 | `chips/drivers/hal/bf0_hal_pinmux.c` |
| 板级 Pinmux | `boards/sf32lb52/helmone/bsp/bsp_pinmux.c` |
| 板级 IO 入口 | `boards/sf32lb52/helmone/bsp/bsp_init.c` |
| UART 注册 | `chips/sf32lb52/sifli_uart.c` |
| I2C 注册 | `boards/.../bsp/sifli_ap.c`、`chips/sf32lb52/sifli_i2c.c` |
| NSH I2C 工具 | `apps/system/i2c/i2c_main.c` |
