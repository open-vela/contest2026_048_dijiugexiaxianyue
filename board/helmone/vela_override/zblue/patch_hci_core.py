#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : external/zblue/zblue / subsys/bluetooth/host/hci_core.c
#   上游 blob: ab23d63b2da40fe5c909e0c083f7fe34a12d197d
#   为什么   : 超时返回 -ETIMEDOUT；skip_sync 轮询退出不可中断等待
#   写入时 HEAD: 6f79fb2a0f83ad49f9fdd10504ab97d6c9eaf6b3
#   版本漂移自查:
#     git -C external/zblue/zblue rev-parse HEAD:<上面的上游文件>   # 与对应 blob 比对
#     git -C external/zblue/zblue diff -- <上面的上游文件>
#   机制：CMake 在本目录下调用本脚本，就地改写 zblue 的构建副本（上游 git 不动）。
# ---------------------------------------------------------------------------
"""Rewrite zblue hci_core.c so HCI command timeout returns an error.

Upstream BT_ASSERT_MSG() on the 10 s Command Complete wait becomes
z_fatal_error → assert.  On SF32 that kills the libuv worker during
ADV stop / scan stop; diag then task_delete and BLE never comes back.

NuttX zblue k_sem_take(timeout) is nxsem_timedwait_uninterruptible.
SIGTERM / task_delete cannot wake it; a clipped wdog list also drops
the 10 s timeout.  Poll K_NO_WAIT + k_msleep so skip_sync / cancel
can abort the wait.

CMake runs this at configure time and compiles the copy instead of
upstream hci_core.c.  Do not copy the whole 5k-line file into git.
"""

from __future__ import annotations

import sys
from pathlib import Path

# MYVENDOR BUILD: patch_hci_core.py 补丁 subsys/bluetooth/host/hci_core.c@ab23d63b2da4 -- 超时返回 -ETIMEDOUT；skip_sync 轮询退出不可中断等待
print('my_vendor: override patch patch_hci_core.py -> subsys/bluetooth/host/hci_core.c@ab23d63b2da4 -- 超时返回 -ETIMEDOUT；skip_sync 轮询退出不可中断等待')

DECL = (
    "/* myvendor: abort HCI wait when LCPU is being recovered */\n"
    "extern bool sf32lb52_bt_hci_skip_sync(void);\n"
)

OLD_SYS = (
    "\t\t\t__maybe_unused bool success = process_pending_cmd(hdev, HCI_CMD_TIMEOUT);\n"
    "\n"
    '\t\t\tBT_ASSERT_MSG(success, "command opcode 0x%04x %s timeout", opcode, bt_hci_opcode_to_str(opcode));\n'
)

NEW_SYS = (
    "\t\t\tif (sf32lb52_bt_hci_skip_sync()) {\n"
    '\t\t\t\tLOG_ERR("command opcode skip (recovering)");\n'
    "\t\t\t\tcmd(buf)->sync = NULL;\n"
    "\t\t\t\tnet_buf_unref(buf);\n"
    "\t\t\t\treturn -ENODEV;\n"
    "\t\t\t}\n"
    "\t\t\t__maybe_unused bool success = process_pending_cmd(hdev, HCI_CMD_TIMEOUT);\n"
    "\n"
    "\t\t\tif (!success) {\n"
    '\t\t\t\tLOG_ERR("command opcode 0x%04x %s timeout", opcode, bt_hci_opcode_to_str(opcode));\n'
    "\t\t\t\tcmd(buf)->sync = NULL;\n"
    "\t\t\t\tnet_buf_unref(buf);\n"
    "\t\t\t\treturn -ETIMEDOUT;\n"
    "\t\t\t}\n"
)

MID_SYS = (
    "\t\t\t__maybe_unused bool success = process_pending_cmd(hdev, HCI_CMD_TIMEOUT);\n"
    "\n"
    "\t\t\tif (!success) {\n"
    '\t\t\t\tLOG_ERR("command opcode 0x%04x %s timeout", opcode, bt_hci_opcode_to_str(opcode));\n'
    "\t\t\t\tcmd(buf)->sync = NULL;\n"
    "\t\t\t\tnet_buf_unref(buf);\n"
    "\t\t\t\treturn -ETIMEDOUT;\n"
    "\t\t\t}\n"
)

OLD_SEM = (
    "\terr = k_sem_take(&sync_sem, HCI_CMD_TIMEOUT);\n"
    "\tBT_ASSERT_MSG(err == 0,\n"
    '\t\t      "Controller unresponsive, command opcode 0x%04x %s timeout with err %d",\n'
    "\t\t      opcode, bt_hci_opcode_to_str(opcode), err);\n"
)

MID_SEM = (
    "\terr = k_sem_take(&sync_sem, HCI_CMD_TIMEOUT);\n"
    "\tcmd(buf)->sync = NULL;\n"
    "\tif (err) {\n"
    '\t\tLOG_ERR("Controller unresponsive, command opcode 0x%04x %s timeout with err %d",\n'
    "\t\t\topcode, bt_hci_opcode_to_str(opcode), err);\n"
    "\t\tnet_buf_unref(buf);\n"
    "\t\treturn err;\n"
    "\t}\n"
)

POLL_SEM = (
    "\terr = 0;\n"
    "\t{\n"
    "\t\tint n;\n"
    "\n"
    "\t\tfor (n = 0; n < 200; n++) {\n"
    "\t\t\tif (sf32lb52_bt_hci_skip_sync()) {\n"
    "\t\t\t\terr = -ENODEV;\n"
    "\t\t\t\tbreak;\n"
    "\t\t\t}\n"
    "\t\t\terr = k_sem_take(&sync_sem, K_NO_WAIT);\n"
    "\t\t\tif (err == 0) {\n"
    "\t\t\t\tbreak;\n"
    "\t\t\t}\n"
    "\t\t\tk_msleep(50);\n"
    "\t\t}\n"
    "\t\tif (n >= 200 && err) {\n"
    "\t\t\terr = -ETIMEDOUT;\n"
    "\t\t}\n"
    "\t}\n"
    "\tcmd(buf)->sync = NULL;\n"
    "\tif (err) {\n"
    '\t\tLOG_ERR("Controller unresponsive, command opcode 0x%04x %s timeout with err %d",\n'
    "\t\t\topcode, bt_hci_opcode_to_str(opcode), err);\n"
    "\t\tnet_buf_unref(buf);\n"
    "\t\treturn err;\n"
    "\t}\n"
)

SKIP_ENTER = (
    "\tif (sf32lb52_bt_hci_skip_sync()) {\n"
    "\t\tnet_buf_unref(buf);\n"
    "\t\treturn -ENODEV;\n"
    "\t}\n"
    "\tk_sem_init(&sync_sem, 0, 1);\n"
)

OLD_ENTER = "\tk_sem_init(&sync_sem, 0, 1);\n"

def _myvendor_stamp(path):
    """在被改写出来的副本里留三行标记（约定见 vela_override/CMakeLists.txt 顶部）。

    为什么要写进**生成的文件**：真正编译的是这份副本（build/myvendor_zblue/myvendor_*），
    只有它自己带标记，构建输出里才会出现 `note: '#pragma message: myvendor override compiled(patch): …'`，
    一眼看出"这份 .o 是补丁产物、基于哪个上游版本"。用 `#pragma message`（note/info）而不是
    `#pragma GCC warning`：量产构建会关警告（`-w` / 全局压制），note 关不掉；裸 `#warning` 更不行
    —— 抽换件 target 带 `-Wno-cpp`，会被静默掉（2026-09-19 实测）。
    """
    _script = 'patch_hci_core.py'
    _up = 'external/zblue/zblue / subsys/bluetooth/host/hci_core.c'
    _blob = 'ab23d63b2da4'
    _why = '超时返回 -ETIMEDOUT；skip_sync 轮询退出不可中断等待'
    _sym = 'patch_hci_core'
    _tag = "vela_override/" + _script + " -- 上游 " + _up + "@" + _blob + " -- " + _why
    _marker = (
        "\n/* myvendor override (build-time patch): " + _script + "\n"
        " *   上游 " + _up + " @ " + _blob + "\n"
        " *   原因 " + _why + " */\n"
        '#pragma message("myvendor override compiled(patch): ' + _tag + ' ")\n'
        "const char myvendor_override_patch_marker_" + _sym + "[]\n"
        '    __attribute__((used, section(".myvendor_marker"))) = "'
        + _tag + '";\n'
    )
    path.write_text(path.read_text(encoding="utf-8") + _marker, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_hci_core.py SRC DST", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    text = src.read_text(encoding="utf-8")

    if DECL not in text:
        needle = "int bt_hci_cmd_send_sync("
        if needle not in text:
            print("hci_core.c: bt_hci_cmd_send_sync not found", file=sys.stderr)
            return 1
        text = text.replace(needle, DECL + "\n" + needle, 1)

    if SKIP_ENTER not in text:
        if OLD_ENTER not in text:
            print("hci_core.c: k_sem_init pattern not found", file=sys.stderr)
            return 1
        text = text.replace(OLD_ENTER, SKIP_ENTER, 1)

    if NEW_SYS not in text:
        if MID_SYS in text:
            text = text.replace(MID_SYS, NEW_SYS, 1)
        elif OLD_SYS in text:
            text = text.replace(OLD_SYS, NEW_SYS, 1)
        else:
            print("hci_core.c: timeout assert pattern not found", file=sys.stderr)
            return 1

    if POLL_SEM not in text:
        if MID_SEM in text:
            text = text.replace(MID_SEM, POLL_SEM, 1)
        elif OLD_SEM in text:
            text = text.replace(OLD_SEM, POLL_SEM, 1)
        else:
            print("hci_core.c: k_sem_take timeout pattern not found", file=sys.stderr)
            return 1

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8")
    _myvendor_stamp(dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
