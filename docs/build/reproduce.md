# 从零复现：拉到源码、编出固件、烧进板子

> 这一页只讲一件事：**别人拿到本参赛仓，怎么编出和作者手上一样的固件、并烧进板子**。
> 板级细节（目录结构 / 配置职责 / LVGL 说明 / 离线构建）见 **[helm-one/README.md](../helm-one/README.md)**。

---

## 0. 这个仓里有什么

| 路径 | 内容 |
| --- | --- |
| `helm-one/` | **作品主体**：完整 vendor 树（板级 + chip 层 + 二级 boot + LVGL 覆盖层 + 构建脚本） |
| `board/helmone/` | 板级适配目录（按赛方"板卡形态"约定放一份，评审可直接看板级代码；内容与 `helm-one/boards/sf32lb52/helmone/` 一致） |
| `app/`、`quickapp/` | 赛方模板占位 |
| `docs/` | 本页与作品文档 · `logs/` AI Coding 日志 |

## 1. 拉工程（openvela 全量 + 本参赛仓）

```bash
repo init -u https://github.com/jinsc123654/contest2026_048_dijiugexiaxianyue \
  -b dev-ai-contest-2026 -m contest2026_048_dijiugexiaxianyue.xml
repo sync -c -j8
```

同步后 openvela 全量源码在工作区根目录，本参赛仓在其
`contest2026_048_dijiugexiaxianyue/` 子目录里；清单里的 `linkfile` 会把
`helm-one/` 挂到工作区的 **`vendor/HelmOne/`** —— 板级 defconfig 的
`CONFIG_ARCH_BOARD_CUSTOM_DIR` 就是按这个路径写的，位置不能改。

> **不用 repo、直接 clone 本仓也可以**（等价做法）：
> ```bash
> git clone -b dev-ai-contest-2026 \
>   https://github.com/jinsc123654/contest2026_048_dijiugexiaxianyue
> ln -s "$PWD/contest2026_048_dijiugexiaxianyue/helm-one" vendor/HelmOne   # 等价于清单里的 linkfile
> ```
> 上游合入后，也可以直接 `repo init -u https://github.com/open-vela/contest2026_048_dijiugexiaxianyue`。

## 2. 编译（一条命令编出可烧录的 SD 整盘镜像）

命令在 **openvela 工作区根目录**执行（那里有 `build.sh` 和 `nuttx/`）：

```bash
bash vendor/HelmOne/docs/tools/make_sd_2g.sh     # 2 GiB 卡；4 / 8 / 16 GiB 各有一个同名脚本
```

它会：编 main 槽固件 → 编 factory 槽固件 → （尝试重建二级 boot，失败则用随仓的
`bootloader.bin` / `ftab.bin`）→ 生成 kv / lfs / fat 文件系统镜像 → 打成与卡容量等大的整盘 `.img`。
默认输出 `boot_loader/bin/helmone_sd_<尺寸>.img`，`-o` 可改；尺寸只支持 `2G`/`4G`/`8G`/`16G`
或 `32768M` 这类写法，**最小 2 GiB**（布局里 FAT 区从 1 GiB 起）。

只想编固件不打整盘镜像，用底层入口：

```bash
./build.sh vendor/HelmOne/boards/sf32lb52/helmone/configs/nsh/ --cmake -j8     # main 槽
bash vendor/HelmOne/scripts/wrap_nuttx_image.sh cmake_out/helmone_nsh          # nuttx.bin -> nuttx.flash.bin
bash vendor/HelmOne/scripts/build_fs_root.sh                                   # kv/lfs/fat 镜像
```

### 2.1 想编得**和作者一模一样**（字节级），注意四点

1. **干净树**：先删掉 `cmake_out/`（或 `nuttx/distclean`）再编。
   本工程的构建树会**静默跳过重编**（源文件时间戳没变就不重编），增量构建出来的
   镜像其实是多次构建的混合物 —— 这一点在开发机上就实际发生过（镜像里同时存在
   两个不同时间的 `__DATE__` 字符串）。
2. **固定时间戳**：`build_date` / `generated_utc` 默认取当前时间。
   设 `SOURCE_DATE_EPOCH=<unix 秒>` 再编（`vendor/HelmOne/scripts/wrap_nuttx_image.py`
   已支持），这一项就会被钉住。
3. **配置**：defconfig 改动只有在**路径变化**时才会被 cmake 重新读取，所以换配置
   时请换输出目录（或用干净的 `cmake_out/`），别依赖原地改 defconfig。
4. **子仓 revision 一致**：本工程依赖 openvela 的多个子仓（nuttx / apps /
   frameworks / external 等）。要字节级一致，务必用同一次 `repo sync` 的 manifest。

### 2.2 关于 LVGL：为什么仓库里有两份

openvela 自带一份 `apps/graphics/lvgl/`，本作品在 `vendor/HelmOne/apps/graphics/lvgl/`
下**又带了一份完整的 LVGL 9.5**。两份都在磁盘上，但**编译时只编一份**：

- `CONFIG_MYVENDOR_LVGL_STACK=y`（`chips/sf32lb52/Kconfig`）→ 板级 `CMakeLists.txt` 把
  vendor 那份 `add_subdirectory(... myvendor_lvgl)` 编成 `liblvgl`；
- 官方 `apps/graphics/lvgl/CMakeLists.txt` 开头 `if(CONFIG_MYVENDOR_LVGL_STACK) return()`，
  官方那份直接退出，否则两个脚本会定义同名 `lvgl` target，CMake 配置直接失败。

之所以要自己的副本，是因为本板改了 LVGL 内部：PSRAM 分配器（`lv_mem_core_clib.c`）、
绘制线程命名（`lv_pthread.c`）、系统 TTF 预载（`myvendor_system_font.c`），
外加本板的 LCD 冲刷口（`lv_myvendor_lcd.c` / `lv_nuttx_myvendor_init.c`）——
这些改动放在 vendor 树里才不会被 `repo sync` 冲掉。
细节（含 Kconfig 补丁、blob 对照、完整 diff）见
[helm-one/docs/pitch/lvgl.md](../helm-one/docs/pitch/lvgl.md) 与
[helm-one/README.md](../helm-one/README.md)。

## 3. 烧录

**SD 卡（地图/轨迹所在）**：

```bash
sudo dd if=boot_loader/bin/helmone_sd_2g.img of=/dev/sdX bs=4M status=progress conv=fsync
```

**板载固件（ftab + 二级 boot + main 槽）**：用 SiFli `sftool`，烧写清单来自
`boot_loader/config/nsh/sftool_param.json` 与 ptab 分区表：

```bash
python3 vendor/HelmOne/scripts/gen_flasher_args.py      # -> cmake_out/helmone_nsh/flasher_args.json
# 再按其 write_flash 清单用 sftool 烧（本板 BOOT_STORAGE=nand ⇒ sftool -m nand）
```

板子进下载模式后烧录约 1 分钟；烧完自动复位，串口 1 000 000 波特可看启动日志。

## 4. 上板自检（30 秒版）

串口敲：

```text
sys            # 时钟/电池/GNSS/传感器/radio 一屏
sys hr         # 心率/踏频/功率与三个槽位状态
```

- 屏幕应显示骑行主页面；按 KEY1 翻页、KEY2 确认（整机无触摸）。
- 插 microSD（内含上面 `make_sd_*.sh` 产出的 LFS/FAT 区）后才能看到地图与轨迹。

## 5. 常见坑

| 现象 | 原因 / 处理 |
| --- | --- |
| `build` 秒过、镜像没变 | 构建树跳过重编：删 `cmake_out/` 重来（见 2.1） |
| CMake 报 `add_library cannot create target "lvgl"` | 官方 `apps/graphics/lvgl/CMakeLists.txt` 的早退被 `repo sync` 冲掉了，见 2.2 与 `helm-one/docs/pitch/lvgl.md` |
| CMake 报 `CMAKE_SYSTEM_PROCESSOR not set` | 换过 Kconfig/defconfig 后残留旧 `.config`：删掉 `cmake_out/<cfg>/` 重新配置 |
| 串口一个字节都没有 | 板子没供电；或监视工具的 DTR/RTS 把芯片按在复位（用 `--dtr 0 --rts 0`） |
| `sftool: Failed to connect to the chip` | 先确认供电与端口被别的进程占用 |
| 地图空白 | SD 里没有 `make_sd_*.sh` 产出的地图分片（全国约 314 MB，按城市打包也可） |
