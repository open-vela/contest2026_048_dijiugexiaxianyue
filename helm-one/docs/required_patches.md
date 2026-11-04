# 干净 OpenVela 树上本板卡的必改点

`vendor/HelmOne` 是独立 git。`repo sync` 一份新的 OpenVela 后，**NuttX / apps / zblue 都是上游干净树**。板级代码（分区、SD、LittleFS grow、`df -h`、SAL、bth4 stub）都在 `vendor/HelmOne` 里，会跟着 vendor 仓库走。

本文件只记：**不在 vendor 里、但本板卡完整功能仍然依赖的上游改动**。漏掉它们时，NSH 往往仍能起来，但 **手机广播 + 扫 HR/CSC/CPS 同时开会失败**，或 CMake 与官方 `lvgl` target 撞车。

替换文件和补丁在 [pitch/](pitch/README.md)（[ble.md](pitch/ble.md)、[lvgl.md](pitch/lvgl.md)）。相关：[ble/ble_sensor.md](ble/ble_sensor.md)、[sd_partition.md](sd_partition.md)、[board_guide.md](board_guide.md)。

---

## 1. 干净树能做什么 / 不能做什么

| 功能 | 干净 `nuttx` + `apps` + `zblue` + 本 vendor | 说明 |
|------|---------------------------------------------|------|
| SD 启动、`/mnt/lfs`、`/mnt/kv`、`df -h` | 可以 | 全在 vendor |
| MTP、LCD、板卡传感器 | 可以 | 全在 vendor |
| Companion 给手机打 **legacy 广播** | 可以 | `sf32lb52_bth4.c` stub LE features（无 Ext Adv 位）+ `CONFIG_BT_EXT_ADV_LEGACY_SUPPORT` + `vela_override/bluetooth/sal_le_advertise_interface.c` |
| 手机已连接时启动 sensor observer | **不行** | SF32 可能拒绝 `LE Set Random Address`；必须保留 zblue `id.c` identity 回退，见第 2 节 |
| 手机未连接时同时广播并重连扫描 | 不再依赖并发 | vendor 按 2000 ms ADV / 1000 ms scan 做互斥时间片；板级 SAL ticket 串行化实际 HCI worker |

`repo sync` 之后用下面命令确认 zblue 是否又变干净：

```bash
git -C external/zblue/zblue status --short
git -C external/zblue/zblue diff -- subsys/bluetooth/host/id.c
```

有 diff 才说明本机还留着必改点。

---

## 2. 必须改：`id.c`（zblue host）

**文件：** `external/zblue/zblue/subsys/bluetooth/host/id.c`  
**函数：** `bt_id_set_scan_own_addr()`  
**CMake 目标：** `zblue`（`libzblue.a`）

SF32 LCPU 在 **正在广播、正在扫描、正在建链、或已有从机连接** 时，HCI `LE Set Random Address` 返回 **Command Disallowed（`-EACCES` / `-13`）**。

上游只在「已经在扫 / 正在 initiating」时忽略这个错误。`bt_le_start_scan` 会走 `CONFIG_BT_PRIVACY` → `bt_id_set_private_addr()`；即使应用层已用时间片停掉手机广播，**已经建立的手机 Peripheral 链路**仍可能让命令返回 `-EACCES`，导致观察者起不来、传感器不能重连。

把 `if (err == -EACCES && (SCANNING || INITIATING))` 改成：**任意 `-EACCES` 都沿用当前 identity，返回 0，让扫描继续。**

```c
		err = bt_id_set_private_addr(hdev, BT_ID_DEFAULT);
		if (err == -EACCES) {
			/* LE Set Random Address is disallowed while advertising,
			 * scanning, initiating, or (on some controllers) while a
			 * peripheral link is up.  Keep the current identity and
			 * still start the observer.
			 */
			LOG_WRN("Set random addr disallowed, scan with identity");
			if (hdev->id_addr[BT_ID_DEFAULT].type == BT_ADDR_LE_PUBLIC) {
				*own_addr_type = BT_HCI_OWN_ADDR_PUBLIC;
			} else {
				*own_addr_type = BT_HCI_OWN_ADDR_RANDOM;
			}

			return 0;
		} else if (err) {
			return err;
		}
```

相对上游大约在 `bt_id_set_scan_own_addr()` 里 `CONFIG_BT_PRIVACY` 分支。

该补丁负责“手机链路存在时仍能启动 observer”；`ble_sensor.c` 的 ADV / scan 时间片只负责“手机断开后仍周期性可发现”，不能替代本补丁。以后可改成与 `vela_override/` 相同的 CMake 抽换（目标名 `zblue`，文件放到 `vela_override/zblue/id.c`），避免改 `external/zblue`。在做成抽换之前，**每次 `repo sync` 后都要重新打这一处。**

整文件和 `git apply` 补丁：[pitch/ble.md](pitch/ble.md)，文件在 [pitch/replace/](pitch/replace/) 与 [pitch/patches/](pitch/patches/)。

---

## 3. 不要改 zblue 的其它文件

这些曾经在工作区出现过，**干净树不需要、也不该再改 zblue**：

| 文件 | 原因 |
|------|------|
| `subsys/bluetooth/host/hci_core.h` 里 `#define BT_DEV_FEAT_LE_EXT_ADV(feat) 0` | 板级 `sf32lb52_bth4.c` 对 `LE_READ_LOCAL_FEATURES` 的应答已经是 `features[0]=0x3f`、`features[1]=0x01`，**没有 Ext Adv bit 12**。再加 `CONFIG_BT_EXT_ADV_LEGACY_SUPPORT=y`，上游宏 `BT_FEAT_LE_EXT_ADV(feat)` 已为 0。 |
| `port/drivers/bluetooth/hci/h4.c` | 本板（`my_vendor_nsh`）**编译库里没有它**（`compile_commands.json` 只有 Vela 的 `service/stacks/zephyr/hci_h4.c` 与 `nuttx/drivers/serial/uart_bth4.c`），所以无需抽换、也不必改。读侧修复落在 Vela 的 `hci_h4.c`（见 `docs/pitch/ble-hci-h4-rx-reopen.md`）；`sf32lb52_bth4.c` + `uart_bth4.c` 是 LCPU→环形缓冲的下半截。 |
| `subsys/bluetooth/host/scan.c` 多打 `LOG_ERR` | 仅调试，不影响能否扫。 |

`hci_core.h` 不能靠 `-I` 覆盖：host 用 `"hci_core.h"`，先搜同目录。

---

## 4. 已经在 vendor 里、新环境不用再补的

| 路径 | 作用 |
|------|------|
| `chips/sf32lb52/sf32lb52_bth4.c` | stub LE features / buffer size；HCI 路径 |
| `boards/.../vela_override/bluetooth/*.c` | CMake 抽换 `libbluetooth`：legacy 广播、扫描 `-EALREADY`、GATT keep_db |
| `boards/.../vela_override/zblue/*.c` | CMake 抽换 `zblue`：`init.c` 幂等、禁止第二条 sysworkq / BT LW WQ、`h4.c` `get_rx` 超时（见 [ble/companion_impl_plan.md](ble/companion_impl_plan.md) §7.4） |
| `boards/.../vela_override/zblue/patch_hci_core.py` | 构建期改写 `hci_core.c`：HCI 超时返回错误；peripheral 连接完成先按原始 RPA 查 pending conn、再回退身份地址（定向回连必需；见 [ble/directed_reconnect.md](ble/directed_reconnect.md)） |
| `boards/.../vela_override/zblue/patch_keys_kv.py` | 构建期改写 `keys.c`/`smp.c`：IRK/LTK 落盘 `/mnt/kv/bt_keys.bin`，开机恢复并 `bt_id_add()` 写入控制器 RAL（RAL 容量 3） |
| `boards/.../vela_override/fs/fs_procfs_mount.c` | `df -h` uint64（不改 `nuttx/fs`） |
| `boards/.../vela_override/fs/fs_fat32util.c` | FatSz16==0 当 FAT32（1 GiB / 32 KiB 簇不被判成 FAT16） |
| `boards/.../vela_override/mtd/ftl.c` | FTL `O_DIRECT` 写成功返回扇区数（上游 leftover 0 → `mkdir` `ENODEV`） |
| `boards/.../vela_override/usbdev/mtp.c` | USB iProduct / iSerialNumber 可写（不改 `nuttx/drivers/usbdev/mtp.c`） |
| `chips/sf32lb52/sf32lb_lfs_super.c` | LittleFS superblock grow（不改 `nuttx/fs/littlefs`） |
| `boot_loader/config/nsh/ptab.sdmmc.json` | SD 分区 |
| `configs/nsh/defconfig` | `CONFIG_BT_EXT_ADV_LEGACY_SUPPORT=y` 等 |

`nuttx/fs/littlefs/`、`nuttx/fs/mount/fs_procfs_mount.c`、`nuttx/drivers/usbdev/mtp.c` **必须保持上游干净。**

---

## 5. 工作区里常见、但不是本板卡启动所必需的脏文件

`repo sync` 后这些可以没有，不影响 SD 启动和 BLE 双角色（在已打第 2 节补丁的前提下）。本树只保留 lvgl 另案；`nshlib` 的 `top -t`、`zlib` CMake、`nuttx/tools/build.sh` 与 CLion/`.idea` 已还原或删除。

| 仓库 | 文件 | 用途 |
|------|------|------|
| `apps` | `graphics/lvgl/CMakeLists.txt`、`Makefile` | 另案 lvgl（本树保留）。打法见 [pitch/lvgl.md](pitch/lvgl.md) |

---

## 6. 新环境检查清单

1. `local_manifests` 已拉 `vendor/HelmOne`（见 [board_guide.md](board_guide.md)）。
2. `boot_loader/storage.conf` 为 `BOOT_STORAGE=sd`（或当前实际介质）。
3. `external/zblue/zblue/subsys/bluetooth/host/id.c` 已有第 2 节的 `-EACCES` 处理（可用 [pitch/](pitch/README.md) 覆盖或打补丁）。
4. `apps/graphics/lvgl/CMakeLists.txt` 已有 `CONFIG_MYVENDOR_LVGL_STACK` 早退（见 [pitch/lvgl.md](pitch/lvgl.md)）。
5. 配置日志里有抽换层的加载行（形如 `my_vendor: libbluetooth uses board sal_le_*.c`、`my_vendor: fs uses board fs_procfs_mount.c`）。
   完整清单见 [pitch/overrides.md](pitch/overrides.md)；**h4 不在其中** —— 本板编译库里没有任何 h4 下游实现，只有 Vela 的 `hci_h4.c`（见 [pitch/ble-hci-h4-rx-reopen.md](pitch/ble-hci-h4-rx-reopen.md)）。
6. 开机：`/mnt/lfs`、`/mnt/kv` 挂上；`ble_companion: advertising started`。
7. 手机已连接时 `test sensor scan` 能出表（验证 `id.c`）；手机断开且 sensor 待重连时日志按 ADV / scan 时间片轮换。失败日志常为 `set scan own addr failed (-13)`。
