# hardware/ — 自研 PCB

本目录放**整机自研 PCB 的 EasyEDA（立创 EDA）专业版工程**，可直接打开查看/修改原理图与
PCB，而不是只给截图。

| 文件 | 内容 |
| --- | --- |
| [`speedometer.eprj2`](speedometer.eprj2) | EasyEDA Pro 工程（原理图 + PCB + BOM 关联） |

```text
大小  59 654 144 字节（约 57 MB）
sha256 6352717d390d036c79f9cd8bd0a31217485097934498f7ecf5437e178912e37f
```

> `.eprj2` 实际是一个 **SQLite 数据库容器**（文件头 `SQLite format 3`），
> 里面按表存放原理图页、PCB 图元、工程历史与预览图。不要用文本编辑器打开。

## 1. 怎么打开

1. 安装 **立创 EDA 专业版**（EasyEDA Pro，Windows / macOS / Linux 均有客户端）；
2. 客户端内「文件 → 打开 → 本地工程」，选中 `speedometer.eprj2`；
3. 首次打开会在本机生成缓存，不影响原文件。

版本要求：工程创建于 2026-07-03，最后编辑 2026-10-28，建议用 **2.2.x 及以上**客户端；
过低版本可能提示工程格式不兼容。

## 2. 板子是什么

- 主控 **SiFli SF32LB52**（量产目标 SF32LB527UD6，16 MB PSRAM、片内 PMU 与充电）
- 显示 **NV3031A-AI** 240×320 RGB565，4 线 QAD-SPI @24 MHz，2.4 英寸**半透半反无触摸屏**
  （背光 PWM = PA01，面板电源 = PA10，未引出 TE）
- 定位 **u-blox MAX-M10S**（UART1，38400 8N1）
- 惯导 **BMI270**（IMU）/ **BMP388**（气压计）/ **MMC5983**（磁力计）
- 存储 **microSD**（三卷：KV 256 MiB / LittleFS 512 MiB / FAT 余量）
- 输入 KEY1 / KEY2 + 电源键；USB-C（供电 / 调试 / MTP）
- 整机为**自研 PCB + 自研外壳 1.0 版**，全部功能验证、稳定性测试与户外实骑都在该板上完成

引脚、外设与板级约定的权威口径见
[`helm-one/docs/gpio_pinmux_guide.md`](../helm-one/docs/gpio_pinmux_guide.md)
与 [`helm-one/docs/product.md`](../helm-one/docs/product.md)。

## 3. 设计验证

该版工程在定型前跑过完整的 **DRC（设计规则检查）与 DFM（可制造性检查）**，并据此迭代了
多轮（电源分割、丝印回退、器件间距、孔环等），最终版通过后投产。检查报告与逐版 PCB 渲染
属于开发过程证据，体量较大未随仓发布；如需复核可联系队长。

## 4. 与固件的关系

硬件与固件的对应关系（哪根脚接什么、哪个配置项对应哪个外设）见：

| 主题 | 文档 |
| --- | --- |
| 引脚交换矩阵 / pinmux | [`helm-one/docs/gpio_pinmux_guide.md`](../helm-one/docs/gpio_pinmux_guide.md) |
| 板级适配全景（defconfig / BSP / 抽换层） | [`docs/build/board_guide.md`](../docs/build/board_guide.md) |
| 分区与三卷文件系统 | [`docs/build/sd_partition.md`](../docs/build/sd_partition.md) |
| 整机状态与按键矩阵 | [`docs/build/system_states.md`](../docs/build/system_states.md) |

## 5. 许可

硬件工程文件随本仓以 **Apache-2.0** 发布；其中使用的第三方元器件符号/封装版权归各自
厂商或立创 EDA 库所有（见 [`THIRD_PARTY.md`](../THIRD_PARTY.md)）。
