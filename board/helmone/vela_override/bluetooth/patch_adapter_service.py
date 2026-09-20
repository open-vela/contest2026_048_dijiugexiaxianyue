#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ---------------------------------------------------------------------------
# 抽换件说明（vela_override，**构建期补丁脚本**：不是同名替换，而是改写上游文件）
#   改的是   : frameworks/connectivity/bluetooth / service/src/adapter_service.c
#   上游 blob: 018189c4cc1e8e11ec671b933d0aba6746dac900
#   为什么   : process_enc_state_change_evt() 的 BLE 分支只 find 不 create，
#              framework 设备表里还没有该地址时 device==NULL，紧接着
#              device_set_connection_state(NULL,…) 去读 device->state（偏移 0xab）
#              直接 MemManage fault —— 改成 find_create（与 BR/EDR 那支、
#              bond-state 处理器一致）+ NULL 兜底
#   写入时 HEAD: 43945bc1d13f3494a79e3e1d79ca8312c4c0f6a0
#   版本漂移自查:
#     git -C frameworks/connectivity/bluetooth rev-parse HEAD:service/src/adapter_service.c
#       # 应等于上面的 blob；变了就先看上游怎么改的（本文件头部引用的三处行号可能漂移）
#     git -C frameworks/connectivity/bluetooth diff -- service/src/adapter_service.c
#       # 上游应保持干净（改动只发生在构建目录的副本里）
#   机制：CMake 在本目录下调用本脚本，生成到构建目录再按文件名顶掉 libbluetooth 里那份。
# ---------------------------------------------------------------------------
"""修 `process_enc_state_change_evt()`：BLE 分支补 find_create + NULL 保护。

现场（2026-09-20，板子固件 202609200749）：配对密钥落盘（`/mnt/kv/bt_keys.bin`）
生效后，手机带着旧绑定重连、链路加密成功，紧接着就 MemManage 崩 + 重启，每次必现。
从 coredump 拿到的现场：

    cfsr=00000082 (DACCVIOL，MMARVALID)   mmfar=000000ab
    pc=0x1010e240 -> device_is_connected()  device.c:309
                    `ldrb.w r0, [r0, #171]`   ← r0 == NULL（0xab 就是 device->state 的偏移）
    lr=0x1010c181 -> device_set_connection_state()  ← process_enc_state_change_evt()
                                                     adapter_service.c:907
                                                     ← handle_security_event

调用链上的 device 来自：

    if (transport == BT_TRANSPORT_BREDR)
        device = adapter_find_create_classic_device(addr);        /* 会创建 */
    else if (transport == BT_TRANSPORT_BLE)
        device = adapter_find_device(addr, BT_TRANSPORT_BLE);     /* 只查，可能是 NULL */

**BR/EDR 那支是 find_create，BLE 这支只 find** —— 于是"framework 还没登记过这个
地址"的加密事件就把 NULL 交给了 device_set_connection_state()。

为什么以前没事：以前重启就丢密钥（`CONFIG_SETTINGS` 关着，host `bt_keys` 只在 RAM），
手机根本不可能在设备侧加密成功 —— 这条路径走不到。落盘恢复之后，手机用**身份地址**
重连并加密，而 framework 的设备表是按连接时那个地址登记的，两边对不上 → NULL。

修法：与 bond-state 处理器（同文件 :845）完全一致 —— BLE 也走
`adapter_find_create_le_device(addr, BT_LE_ADDR_TYPE_PUBLIC)`，并在后面补一道
NULL 保护（find_create 失败也不至于再崩）。

自检：`grep -n "adapter_find_create_le_device" cmake_out/<cfg>/myvendor_bt/adapter_service.c`
应看到 :845 与 process_enc_state_change_evt 里各一处；上面的上游 git 应保持干净。
"""

import sys
from pathlib import Path

MARK = "MYVENDOR BUILD: patch_adapter_service.py"

SIG = "static void process_enc_state_change_evt("
FIND_LINE = "        device = adapter_find_device(addr, BT_TRANSPORT_BLE);"
CREATE_LINE = (
    "        /* my_vendor: 这里以前只 find —— framework 设备表里还没这个地址时\n"
    "         * device 是 NULL（例：落盘密钥恢复后手机用**身份地址**重连并加密），\n"
    "         * 紧随其后的 device_set_connection_state(NULL, …) 会在\n"
    "         * device_is_connected() 里 `ldrb r0,[r0,#0xab]` 直接 MemManage fault\n"
    "         * （现场 cfsr=0x82 mmfar=0xab）。与 BR/EDR 那支和 bond-state\n"
    "         * 处理器一致，改成 find_create。 */\n"
    "        device = adapter_find_create_le_device(addr, BT_LE_ADDR_TYPE_PUBLIC);"
)
GUARD_ANCHOR = "    if (encrypted) {"
GUARD = (
    "    if (device == NULL) {\n"
    "        /* my_vendor: 兜底 —— 宁可这次不更新状态，也不要拿 NULL 去解引用。 */\n"
    "        adapter_unlock();\n"
    "        return;\n"
    "    }\n"
)

MARKER = (
    "\n/* myvendor override (build-time patch): patch_adapter_service.py\n"
    " *   上游 frameworks/connectivity/bluetooth / service/src/adapter_service.c\n"
    " *   原因 加密状态变化处理器 BLE 分支 NULL 设备保护（find_create + guard） */\n"
    '#pragma message("myvendor override compiled(patch): '
    "vela_override/bluetooth/patch_adapter_service.py -- 上游 "
    'frameworks/connectivity/bluetooth / service/src/adapter_service.c -- '
    '加密状态变化处理器 BLE 分支 NULL 设备保护")\n'
    "const char myvendor_override_patch_marker_adapter_service[]\n"
    '    __attribute__((used, section(".myvendor_marker"))) = "'
    "vela_override/bluetooth/patch_adapter_service.py -- 上游 "
    "frameworks/connectivity/bluetooth / service/src/adapter_service.c -- "
    '加密状态变化处理器 BLE 分支 NULL 设备保护";\n'
)


def safe_path(raw: str) -> Path:
    if ".." in Path(raw).parts:
        raise SystemExit("refusing path containing '..': %s" % raw)
    p = Path(raw).resolve()
    if p.is_dir():
        raise SystemExit("expected a file path, got a directory: %s" % p)
    return p


def patch(text: str) -> str:
    i = text.find(SIG)
    if i < 0:
        raise SystemExit("process_enc_state_change_evt not found")
    j = text.find("\n}", i)
    if j < 0:
        raise SystemExit("process_enc_state_change_evt body end not found")

    body = text[i:j]
    if FIND_LINE not in body:
        raise SystemExit("BLE find line not found in process_enc_state_change_evt")
    body = body.replace(FIND_LINE, CREATE_LINE, 1)

    if GUARD_ANCHOR not in body:
        raise SystemExit("guard anchor (if (encrypted)) not found")
    body = body.replace(GUARD_ANCHOR, GUARD + GUARD_ANCHOR, 1)

    return text[:i] + body + text[j:]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_adapter_service.py SRC DST", file=sys.stderr)
        return 2

    src_path = safe_path(sys.argv[1])
    dst_path = safe_path(sys.argv[2])
    text = src_path.read_text(encoding="utf-8")

    if MARK in text:
        dst_path.write_text(text, encoding="utf-8")
        print("my_vendor: adapter_service.c already patched")
        return 0

    out = patch(text)
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_text(out + MARKER, encoding="utf-8")
    print("my_vendor: override patch patch_adapter_service.py -> "
          "service/src/adapter_service.c -- 加密状态变化处理器 BLE 分支 NULL 保护")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
