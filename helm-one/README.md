# Helm One — vendor 树（openvela / SiFli SF32LB52）

「Helm One」开源骑行码表的主体源码。本目录是 **openvela 的一个独立 vendor**
（与官方 `vendor/sifli/` 同级）：板级适配、二级 boot、LVGL 覆盖层、地图与字体的构建脚本都在这里。

参赛仓里本目录名为 `helm-one/`，`repo sync` 时由清单
（`contest2026_048_dijiugexiaxianyue.xml`）的 `linkfile` 落到工作区的 **`vendor/HelmOne/`** —— 
板级 defconfig 里的 `CONFIG_ARCH_BOARD_CUSTOM_DIR` 就是按这个路径写死的，两者必须对上。

## 快速开始：一条命令编出可烧录的 SD 整盘镜像

```bash
# 在 openvela 工作区根目录（有 build.sh 和 nuttx/ 的那一层）执行
bash vendor/HelmOne/docs/tools/make_sd_2g.sh     # 2 GiB 卡
bash vendor/HelmOne/docs/tools/make_sd_4g.sh     # 4 / 8 / 16 GiB 各有一个同名脚本
```

脚本按顺序做三件事：编 main + factory 两个配置 → 生成 kv / lfs / fat 文件系统镜像 →
打成与卡容量等大的整盘 `.img`（默认落在 `boot_loader/bin/`，`-o` 可指定路径）。
产物直接写卡：

```bash
sudo dd if=boot_loader/bin/helmone_sd_2g.img of=/dev/sdX bs=4M status=progress conv=fsync
```

只想编固件、不打整盘镜像：`bash vendor/HelmOne/make_sd_img.sh 8G`（尺寸写法 `2G`/`32768M`，
最小 2 GiB —— 布局里 FAT 区从 1 GiB 起）。从零拉工程到点灯的完整步骤见参赛仓的
[`docs/build/reproduce.md`](../docs/build/reproduce.md)。

## 目录结构

```
helm-one/
├── make_sd_img.sh              # 一条命令：编固件 + 打 SD 整盘 .img
├── apps/graphics/lvgl/         # 本板自有的一份 LVGL 9.5（见下方"为什么"）
├── boards/sf32lb52/
│   ├── drivers/                # LCD / 触摸
│   └── helmone/                # 板级（与 sf32lb52_devkit_lcd 同构）
│       ├── configs/nsh/        # defconfig（+ nsh-driver / nsh-factory）
│       ├── scripts/            # ld.script, Make.defs
│       ├── include/            # 板级公共头
│       ├── vela_override/      # Vela 上游文件抽换
│       ├── bsp/                # BSP + 启动编排
│       ├── ctl/                # 系统控制（串口命令）
│       ├── services/           # BLE / MTP / 传输
│       ├── domain/             # GPX / 字体
│       ├── ui/                 # 自行车 UI
│       └── nsh/                # NSH / 探针
├── boot_loader/                # NAND 启动链（sifli 无）
│   ├── build.sh
│   ├── bin/                    # bootloader.bin / ftab.bin（预置；其余为构建产物）
│   ├── config/nsh/             # ptab.json, sftool_param.json, boot.json
│   ├── include/                # ptab.h, custom_mem_map.h（分区宏）
│   └── scripts/                # gen_ftab.py, load_boot_config.py
├── chips/sf32lb52/             # chip 层（Kconfig / 板级扩展）
├── middleware/
├── scripts/                    # wrap_nuttx_image.sh, build_fs_root.sh, pack_sd_img.py …
├── .cache/                     # littlefs 源码缓存（随仓发布，离线也能构建）
└── docs/
    ├── pitch/                  # 上游必改件：替换文件、补丁、改法
    └── tools/make_sd_{2,4,8,16}g.sh
```

## 配置职责划分

| 位置 | 内容 | 类比 sifli |
|------|------|------------|
| `boards/.../configs/nsh/defconfig` | NuttX 内核/驱动 Kconfig | 相同 |
| `boards/.../scripts/` | 链接脚本、Make.defs | 相同 |
| `boot_loader/config/nsh/ptab.json` | Flash 分区表 | sifli 无（NOR XIP 不需） |
| `boot_loader/config/nsh/sftool_param.json` | sftool 烧录清单 | sifli 无 |
| `boot_loader/include/` | 分区地址 C 宏（`ptab.h`） | sifli 无 |
| `cmake_out/helmone_nsh/nuttx.bin` | 主固件 | `cmake_out/..._nsh/nuttx.bin` |

## defconfig 路径

| 配置项 | 指向 |
|--------|------|
| `ARCH_BOARD_CUSTOM_DIR` | `vendor/HelmOne/boards/sf32lb52/helmone` |
| `ARCH_CHIP_CUSTOM_DIR` | `vendor/HelmOne/chips/sf32lb52` |

## 为什么这里有一份完整的 LVGL（`apps/graphics/lvgl/`）

openvela 自带 `apps/graphics/lvgl/`（清单里的 `apps_graphics_lvgl` 子仓）；本目录下
**也有一份完整的 LVGL 9.5**。两份同时存在于磁盘上，但**编译时只会编一份**：

| 层 | 作用 |
| --- | --- |
| `configs/nsh/defconfig` | `CONFIG_MYVENDOR_LVGL_STACK=y`（定义在 `chips/sf32lb52/Kconfig`，`imply GRAPHICS_LVGL`，`CONFIG_LV_*` 仍由官方 apps 的 Kconfig 推导） |
| 板级 `CMakeLists.txt` | `add_subdirectory(.../apps/graphics/lvgl myvendor_lvgl)` —— 把本树这份编成 `liblvgl` |
| 官方 `apps/graphics/lvgl/CMakeLists.txt` | 开头 `if(CONFIG_MYVENDOR_LVGL_STACK) return()` —— 官方那份**直接退出**，不再定义同名 `lvgl` target |

**为什么不直接改官方那份**：本板要动 LVGL 内部三处 ——
`src/stdlib/clib/lv_mem_core_clib.c`（`CONFIG_MYVENDOR_LVGL_PSRAM_MALLOC` 走 PSRAM 分配器）、
`src/osal/lv_pthread.c`（给绘制线程命名）、外加 `myvendor_system_font.c`（预载系统 TTF）；
再加上本板的 LCD 冲刷口 `lv_myvendor_lcd.c` / `lv_nuttx_myvendor_init.c`。
这些改动都长在 vendor 树里、跟着本仓走，`repo sync` 冲不掉。两份同名 target 会直接让 CMake
配置失败（`add_library cannot create target "lvgl"`），所以官方入口必须早退。

**另外还有一个 LVGL 的 Kconfig 补丁**：`CONFIG_LV_*` 这一堆符号是 cmake 从**官方**
`apps/graphics/lvgl/Kconfig` 里读的（`osource "$APPSDIR/graphics/lvgl/lvgl/Kconfig"`），
和真正参与编译的源码不是一份；官方那版是旧 LVGL 的，缺 `LV_SUNDAY_STR` 等新符号
（LVGL 9.5 的 `lv_calendar.c` 要用），必须把官方 Kconfig 更新到 9.5。

两处上游改动（`CMakeLists.txt` / `Makefile` 早退 + `lvgl/Kconfig` 更新）的完整 diff、blob 对照与
验证方法见 **[docs/pitch/lvgl.md](docs/pitch/lvgl.md)**；本板所有上游改动汇总见
[docs/pitch/README.md](docs/pitch/README.md)。

## 离线构建

`.cache/` 里预置了两份 littlefs 源码（`littlefs-nuttx` 给 NuttX 侧、`littlefs-v2.5.1` 给
`scripts/build_fs_root.sh` 编 `mklfs_disk`）。`build_fs_root.sh` 只在目录缺失时才去 GitHub clone，
所以**带着这两份缓存，无网络也能编出 SD 镜像**；这两条路径是清单里"跟踪它只会让 `git status` 常年脏"
的例外，其余构建缓存（`cmake_out/`、`*.img`）仍不进版本库。

## 文档

- [上游必改件 pitch](docs/pitch/README.md)：`repo sync` 会冲掉的上游改动（BLE / LVGL / HCI）
- [LVGL 栈与官方脚本避让](docs/pitch/lvgl.md)
- [构建脚本入口](docs/tools/)：`make_sd_{2,4,8,16}g.sh`

## 参考

- 官方只读：`vendor/sifli/`
- 参赛仓总说明与技术报告：本仓根目录 `README.md`、`docs/`
