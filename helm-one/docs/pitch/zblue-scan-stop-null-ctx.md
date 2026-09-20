# Vela 低层改动：scan-stop 在"适配器拆除窗口"里的空指针（板子 panic 根因）

> 本文件属于「上游必改件」记录（见 [README.md](README.md)）：改动**不在 vendor 编译单元里**，
> 改的是 zblue 自己的主机栈文件，`repo sync` 之后必须重新应用。
>
> 对应补丁：[patches/zblue-scan-stop-null-ctx.patch](patches/zblue-scan-stop-null-ctx.patch)

## 改哪个组件、基于哪个版本（**换版本前一定先对这张表**）

| 项 | 值 |
|---|---|
| 仓库 | `external/zblue/zblue`（独立 repo 工程 `zblue`） |
| 写入时的 HEAD | `6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3`（`6f79fb2a0f8`，2026-04-22 `chore: sync .github…`） |
| 文件 | `subsys/bluetooth/host/scan.c` |
| 该文件在 HEAD 的 blob | `8d6ff66b5ff51cb75908cb2e6a06aa4398401a0c` |
| 补丁写于 | 2026-09-19 |

**怎么判断是否需要重新适配**（版本漂移时最省事的三步）：

```bash
git -C external/zblue/zblue rev-parse HEAD
git -C external/zblue/zblue rev-parse HEAD:subsys/bluetooth/host/scan.c   # 应等于上表 blob
git -C external/zblue/zblue apply --check \
    ../../../vendor/my_vendor/docs/pitch/patches/zblue-scan-stop-null-ctx.patch
```

- HEAD 变了但 blob 没变 ⇒ 文件没被动过，**照着下面"改后"直接改这两处**即可；
- blob 也变了 ⇒ 先 `git log -p -1 <blob>..HEAD -- subsys/bluetooth/host/scan.c` 看上游怎么改的，
  再按本文件的意图重做（判空仍应加在 `bt_scan_softreset()` 入口）。

## 现场（实机 crash note，2026-09-18 20:35）

`/mnt/kv/…` 里的 `n007_20260918_203502.txt`：

```
kind=assert   file=../../nuttx/arch/arm/src/arm_m/arm_memfault.c  line=136
pid=48 name=ble_companion
cfsr=00000082   mmfar=00000004        ← DACCVIOL：读/写地址 0x4
pc=10106aa6     lr=101071cd
```

用当次镜像的 ELF（`/home/jinsc/SDK/vela/elf/my_vendor+nsh+20260918-172158+892bab5d.elf`）解出来：

```
pc → bt_scan_softreset      scan.c:111
lr → bt_le_scan_stop_mc     scan.c:1841
```

`mmfar=0x4` 正是 `struct bt_dev_scan_ctx` 里 `scan_dev_found_cb` 的偏移 ⇒
**`hdev->scan_ctx == NULL`，代码在往空指针 +4 写**。

同一时刻的串口日志：

```
[2344.710] ble_companion: adapter off done, enable after 8000 ms      ← 栈已被拆
[coredump] assert … arm_memfault.c:136 irq=1 writing kv
```

以及更长一段里已有的征兆：

```
[2302.5] wdog: clipped corrupt g_wdactivelist (1) …（waitdog 节点被写坏，两个任务被摘）
[2357.0] myvendor net_buf: pool 0x20001c78 count=4 empty 2000 ms, giving up (n=0)
[2280-2365] sal adv ticket n=1825 serving=1810   ← 票据序号在涨、serving 冻住：
                                                    SAL 里压着一批 GAP 工作项没跑完
```

## 机制

`hdev->scan_ctx` 的**唯一赋值点**是 `bt_scan_reset()`（`scan.c:121`），它只在两处被调用：
`hci_reset_complete()`（`hci_core.c:2523`，**HCI_RESET 完成之后**）与 `bt_finalize_init()`
（`hci_core.c:4333`）。而设备结构在初始化路径上会被 `memset(hdev, 0, sizeof(*hdev))`
（`hci_core.c:187`）清零，`scan_ctx` 随之变 NULL。

于是**"适配器已关、HCI_RESET 还没回来"这个窗口**里，任何一次 scan-stop 都会踩空。
上游对 **start** 有保护（`bt_le_scan_start_mc`：`if (!atomic_test_bit(hdev->flags, BT_DEV_READY))
return -EAGAIN;`），**stop 这条漏了**。

这个窗口是真实存在的：板级 `adapter cycle`（恢复阶梯第一级）就是"disable → 等 8 s → enable"，
而 SAL 里排队的工作项会在 disable 之后才被 worker 执行 —— 上面的 `serving=1810` 就是证据，
**从上层堵不干净**，所以修在栈里。

## 改动前（上游 HEAD 原文）

注意：`bt_le_scan_stop_mc()` 里紧随 `bt_scan_softreset()` 之后的
`hdev->scan_ctx->scan_dev_found_cb = NULL;` 是**同一窗口的第二颗雷**
（同一条路径，所以那次先崩在前面那句）。

`subsys/bluetooth/host/scan.c`（blob `8d6ff66b5ff`）：

```c
void bt_scan_softreset(struct bt_dev *hdev)
{
	hdev->scan_ctx->scan_dev_found_cb = NULL;
#if defined(CONFIG_BT_EXT_ADV)
	reset_reassembling_advertiser(hdev);
#endif
}
```

```c
int bt_le_scan_stop_mc(uint8_t dev_id)
{
	struct bt_dev *hdev = bt_dev_get(dev_id);
	if (!hdev) {
		return -ENODEV;
	}

	bt_scan_softreset(hdev);
	hdev->scan_ctx->scan_dev_found_cb = NULL;

	if (IS_ENABLED(CONFIG_BT_EXT_ADV) &&
	    atomic_test_and_clear_bit(hdev->flags, BT_DEV_SCAN_LIMITED)) {
		atomic_clear_bit(hdev->flags, BT_DEV_RPA_VALID);

#if defined(CONFIG_BT_SMP)
		bt_id_pending_keys_update(hdev);
#endif
	}

	return bt_le_scan_user_remove(hdev, BT_LE_SCAN_USER_EXPLICIT_SCAN);
}
```

## 改动后（完整函数，可直接照抄）

```c
void bt_scan_softreset(struct bt_dev *hdev)
{
	/* 板级改动（my_vendor，见 docs/pitch/zblue-scan-stop-null-ctx.md）：
	 * `hdev->scan_ctx` 只在 HCI_RESET 完成（hci_reset_complete → bt_scan_reset）
	 * 或 bt_finalize_init 里被赋值，设备结构在初始化路径上会被 memset 清零。
	 * 于是"适配器已拆掉、复位还没回来"这个窗口里再来一次 scan-stop，就是
	 * hdev->scan_ctx == NULL 解引用 —— 实机 2026-09-18 20:35 的 panic 正是它
	 * （MemManage cfsr=0x82 mmfar=0x4，pc=bt_scan_softreset scan.c:111，
	 *  lr=bt_le_scan_stop_mc scan.c:1841，task=ble_companion）。
	 *
	 * 调用方可能来自 SAL 里排队的工作项（适配器 cycle 拆栈时队列还没清空），
	 * 从上层堵不干净，所以在这里直接变 no-op。 */
	if (hdev == NULL || hdev->scan_ctx == NULL) {
		LOG_WRN("scan softreset skipped, scan ctx not ready");
		return;
	}

	hdev->scan_ctx->scan_dev_found_cb = NULL;
#if defined(CONFIG_BT_EXT_ADV)
	reset_reassembling_advertiser(hdev);
#endif
}
```

```c
int bt_le_scan_stop_mc(uint8_t dev_id)
{
	struct bt_dev *hdev = bt_dev_get(dev_id);
	if (!hdev) {
		return -ENODEV;
	}

	bt_scan_softreset(hdev);

	/* 同一窗口的第二处解引用（scan.c:1856）：softreset 已经判过 NULL，
	 * 那次没 panic 只是因为同一路径，但这里同样不能裸写。 */
	if (hdev->scan_ctx != NULL) {
		hdev->scan_ctx->scan_dev_found_cb = NULL;
	}

	if (IS_ENABLED(CONFIG_BT_EXT_ADV) &&
	    atomic_test_and_clear_bit(hdev->flags, BT_DEV_SCAN_LIMITED)) {
		atomic_clear_bit(hdev->flags, BT_DEV_RPA_VALID);

#if defined(CONFIG_BT_SMP)
		bt_id_pending_keys_update(hdev);
#endif
	}

	return bt_le_scan_user_remove(hdev, BT_LE_SCAN_USER_EXPLICIT_SCAN);
}
```

- 语义：**没初始化好的扫描上下文本来就没有"回调要清"这回事**，跳过即正确。
- `LOG_WRN` 在本配置下**可能被编译掉**（不影响判空逻辑）：确认改动是否进了镜像要看反汇编，
  别看日志里有没有那句话。

## 取舍与不做的部分

- 只堵 **stop 路径**（本次故障路径）。`scan.c` 内部还有 30 余处 `hdev->scan_ctx->`
  裸解引用，逐个加判空没有意义 —— 真正的约束是"不要在窗口里调进来"，
  而 start 侧上游已有 `BT_DEV_READY` 门，stop 侧补上就够。
- 更治本的方向（未做）：让 adapter cycle **在 disable 之前等 SAL 队列排空**，
  或让 SAL 在拆栈时取消排队的工作项。前者在 Vela 侧；后者可做在板级 SAL 抽换件
  （`vela_override/bluetooth/sal_le_{scan,advertise}_interface.c`，属于本仓库，不进 pitch）。

## 验证

构建后确认判空真的在目标码里（推荐，因为 `LOG_WRN` 可能不打印）：

```bash
O=$(find cmake_out/my_vendor_nsh -name scan.c.o -path "*zblue*" | head -1)
prebuilts/gcc/linux-x86_64/arm-none-eabi/bin/arm-none-eabi-objdump -d "$O" |
  awk '/<bt_scan_softreset>:/{f=1} f{print; n++} n>8{exit}'
```

期望两个 `cbz`（`hdev` 与 `scan_ctx`）挡在 `str [r2, #4]` 前面：

```
00000000 <bt_scan_softreset>:
   0:	cbz	r0, 16
   2:	ldr.w	r2, [r0, #692]	@ 0x2b4   ← scan_ctx
   6:	cbz	r2, 16
   e:	str	r1, [r2, #4]            ← 原来崩在这里
```

实机：进入一次 `adapter cycle`（码表断电再上电，或 `ctl radio off` → `on` 触发恢复阶梯），
不该再出现 `arm_memfault.c … mmfar=00000004` 的 panic。

## 回退

`git -C external/zblue/zblue checkout -- subsys/bluetooth/host/scan.c`，或删掉那两个判空。
