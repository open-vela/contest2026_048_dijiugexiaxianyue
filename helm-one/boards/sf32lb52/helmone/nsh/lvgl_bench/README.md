# lvgl_bench

在 SF32LB52 的实际 LCD 上运行 LVGL 官方 benchmark；RGB565、双 SRAM 条带、PARTIAL 刷新。测试会自动轮换 16 个场景，约 70–90 秒，结束后显示结果表并输出 CSV。它测量 LVGL 绘制与显示，不覆盖地图解码/光栅化、业务逻辑或中文 FreeType 首次加载。

## 准备与恢复

在 openvela 根目录执行：

```sh
python3 vendor/HelmOne/scripts/configure_lvgl_bench.py
./vela_my_vendor_tools.py build flash monitor -p /dev/ttyACM0
```

脚本在现有构建配置上开启 benchmark、perf monitor 和 LCD 统计，关闭 bicycle 自动启动，并设置 16 ms 刷新、1 ms 系统 tick、100 tick 时间片；首次保存 `.config.pre-lvgl-bench`。不修改产品 defconfig，也不擦除地图/用户数据。USB 重新枚举后应核对 `/dev/serial/by-id/`，不要只凭 ACM 序号判断设备。

排查显示异常时可改用 `python3 vendor/HelmOne/scripts/configure_lvgl_bench.py --no-epic`：关闭 LVGL EPIC、板级 EPIC 和队列模式，地图搬运回退 CPU；其余 LCD 时钟与刷屏配置保持不变。编译、烧录后运行相同的 `lvgl_bench &`，并直接观察屏幕是否仍异常。

要在正常业务界面上复核，使用 `python3 vendor/HelmOne/scripts/configure_lvgl_bench.py --restore --no-epic` 后重新编译烧录。这样恢复 bicycle 自动启动，同时保持 EPIC 关闭。最终使用 `--restore` 恢复产品默认配置。

恢复产品配置，再重新编译烧录：

```sh
python3 vendor/HelmOne/scripts/configure_lvgl_bench.py --restore
./vela_my_vendor_tools.py build flash monitor -p /dev/ttyACM0
```

## 板端命令

```sh
ctl log info
ctl dvfs 240
lvgl_bench --cache=filtered &
```

同一启动周期只运行一次；结束后需重启再测。不要在 bicycle 已运行时启动，不能通过 kill bicycle 安全地重用 LVGL 全局状态。
其他轮次可分别运行 `lvgl_bench spin --cache=filtered &` 或 `lvgl_bench fast --cache=filtered &`。

- `--cache=filtered`（默认）：跳过 MPU 明确标为非缓存 SRAM 的维护，保留内存屏障；PSRAM/未知范围仍维护。
- `--cache=legacy`：同一测试固件恢复原来的缓存维护调用，用于 A/B。仅 benchmark 构建提供此开关。
- `spin`：保留 LCD 传输，以忙等方式按刷新节拍推进，排查 NuttX 睡眠粒度造成的限速。
- `fast`：跳过 LCD 传输，并把刷新与动画定时器调到 1 ms，测绘制路径上限；结果不能代表屏幕实际帧率。
- `EPIC_CACHE`：执行的 clean/flush/invalidate 次数、跳过次数、请求/跳过字节数。覆盖 EPIC 图层维护和 NuttX 实际安装的 draw buffer 回调。字节数是维护请求的累计范围，不是实际总线流量。
- `Benchmark Summary` 与串口 CSV：每场景的 CPU、FPS、render/flush 毫秒数；同时输出 EPIC 接受/拒绝任务及各绘制路径耗时。
- `LVGL_BENCH_DONE`：该轮结束。结果页面保持显示，NSH 可继续操作。

每次重启后重新固定 `ctl dvfs 240`。保持同样的日志级别、刷新周期、连接状态与场景字体，至少重复两次。`lcd_recover status` 查看 flush 错误/超时；保存全部串口日志，剔除有重启或故障的轮次，不隐藏异常。

测试结束恢复原日志与调频策略：`ctl log err`、`ctl dvfs auto`（日志配置会保存到 KV）。

## 编译优化对照

```sh
python3 vendor/HelmOne/scripts/configure_lvgl_bench.py --opt-level=-O2
./vela_my_vendor_tools.py build flash monitor -p /dev/ttyACM0
```

只修改 `LV_OPTLEVEL`，不改全工程优化等级；`--opt-level=-O1` 可切回。检查实际 `compile_commands.json`，不要只看 Kconfig。默认 `keep` 保留当前设置。

## 限制

当前单 UI 线程、同步 EPIC HAL；地图和 LVGL 共用硬件。在统一 GPU 仲裁及缓冲生命周期之前，不应直接打开 EPIC draw thread 或增加软件 draw unit 数量。

16 ms 刷新周期仍会限制轻场景 FPS；优先比较重场景 render 时间，不将各场景平均 FPS 当作产品页面帧率。sysmon 的 CPU 也不等同于 GPU 利用率。默认 benchmark 栈 48 KiB。
