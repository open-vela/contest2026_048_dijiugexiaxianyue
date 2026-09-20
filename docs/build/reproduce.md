# 从零复现：拉到源码、编出固件、烧进板子

> 这一页只讲一件事：**别人拿到本参赛仓，怎么编出和作者手上一样的固件**。
> 日常开发命令见 `vela_my_vendor_tools.md`；板级细节见 `board_guide.md`。

---

## 1. 拉工程（openvela 全量 + 本参赛仓）

```bash
repo init -u https://github.com/open-vela/contest2026_048_dijiugexiaxianyue \
  -b dev-ai-contest-2026 -m openvela.xml
repo sync -c -j8
```

同步后 openvela 全量源码在工作区根目录，本参赛仓在其
`contest2026_048_dijiugexiaxianyue/` 子目录里。

## 2. 引入自研 vendor 树（主体代码）

主体代码在 `vendor/my_vendor/`，**不在本仓内**，由 Gitee 提供：

```bash
mkdir -p .repo/local_manifests
cp contest2026_048_dijiugexiaxianyue/board/my_vendor.xml .repo/local_manifests/
repo sync -c -j8 vendor/my_vendor
```

等价做法：`git clone https://gitee.com/jinsc123654/vela_sifli vendor/my_vendor`。

> **要复现"某一次"固件**：把 `my_vendor.xml` 里的 `revision="master"`
> 换成作者给出的 commit（`git -C vendor/my_vendor rev-parse HEAD`），
> 再 `repo sync`，这样拿到的就是那一版源码，而不是最新的 master。

## 3. 编译

全部命令在 **openvela 工作区根目录**执行（那里有 `build.sh` 和 `nuttx/`）：

```bash
python3 vendor/my_vendor/build_board.py build        # 产品固件（main 槽）
python3 vendor/my_vendor/build_board.py build-all    # main + factory + 二级 boot
python3 vendor/my_vendor/build_board.py pack-sd-img  # SD 整盘镜像（含 LFS 种子）
```

### 3.1 想编得**和作者一模一样**（字节级），注意四点

1. **干净树**：先删掉 `cmake_out/`（或 `nuttx/distclean`）再编。
   本工程的构建树会**静默跳过重编**（源文件时间戳没变就不重编），增量构建出来的
   镜像其实是多次构建的混合物 —— 这一点在开发机上就实际发生过（镜像里同时存在
   两个不同时间的 `__DATE__` 字符串）。
2. **固定时间戳**：`build_date` / `generated_utc` 默认取当前时间。
   设 `SOURCE_DATE_EPOCH=<unix 秒>` 再编（`vendor/my_vendor/scripts/wrap_nuttx_image.py`
   已支持），这一项就会被钉住。
3. **配置**：defconfig 改动只有在**路径变化**时才会被 cmake 重新读取，所以换配置
   时请换输出目录（或用干净的 `cmake_out/`），别依赖原地改 defconfig。
4. **子仓 revision 一致**：本工程依赖 openvela 的多个子仓（nuttx / apps /
   frameworks / external 等）。要字节级一致，务必用同一次 `repo sync` 的 manifest。

> 端口与烧录参数（`-p /dev/ttyACM0`、`-M nand`、波特率等）见 `vela_my_vendor_tools.md`。

## 4. 烧录

```bash
python3 vendor/my_vendor/build_board.py flash                 # ftab + 二级 boot + main
python3 vendor/my_vendor/build_board.py flash-all             # 全部条目
python3 vendor/my_vendor/build_board.py burn-sd --sd /dev/sdX  # 整盘 SD（含文件系统）
```

板子进下载模式后烧录约 1 分钟；烧完自动复位，串口 1 000 000 波特可看启动日志。

## 5. 上板自检（30 秒版）

```text
sys            # 时钟/电池/GNSS/传感器/radio 一屏
sys hr         # 心率/踏频/功率与三个槽位状态
```

- 屏幕应显示骑行主页面；按 KEY1 翻页、KEY2 确认（整机无触摸）。
- 插 microSD（内含 `pack-sd-img` 产出的 LFS/FAT 区）后才能看到地图与轨迹。

## 6. 常见坑

| 现象 | 原因 / 处理 |
| --- | --- |
| `build` 秒过、镜像没变 | 构建树跳过重编：删 `cmake_out/` 重来（见 3.1） |
| 串口一个字节都没有 | 板子没供电；或监视工具的 DTR/RTS 把芯片按在复位（用 `--dtr 0 --rts 0`） |
| `sftool: Failed to connect to the chip` | 先确认供电与端口被别的进程占用；烧录时会自动"让出/收回"串口 |
| 地图空白 | SD 里没有 `pack-sd-img` 产出的地图分片（全国约 314 MB，按城市打包也可） |
