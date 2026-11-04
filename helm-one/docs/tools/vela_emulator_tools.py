#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
openvela Vela Emulator 构建与运行辅助脚本（用法类似 vela_sifli_tools.py）。

文档:
  docs/zh-cn/quickstart/openvela_ubuntu_quick_start.md

用法（在 openvela 根目录或任意子目录执行）:
  python3 vela_emulator_tools.py demo           # 列出可选 Demo（编号数组）
  python3 vela_emulator_tools.py menuconfig
  python3 vela_emulator_tools.py build
  python3 vela_emulator_tools.py run
  python3 vela_emulator_tools.py build run
  python3 vela_emulator_tools.py build -d 2     # 按编号配置并编译 Demo
  python3 vela_emulator_tools.py run -d 2       # 推送资源并提示运行命令
  python3 vela_emulator_tools.py debug          # 以 GDB stub 启动（-qemu -S -s）
  python3 vela_emulator_tools.py run -- -no-window

Tab 补全（bash）:
  eval "$(python3 vela_emulator_tools.py complete bash)"
  python3 vela_emulator_tools.py complete install
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

# =============================================================================
# 工程配置 — 切换模拟器目标时主要改这里
# =============================================================================

# 相对 openvela 根目录的板级 config 路径（末尾可有可无 /）
BOARD_CONFIG = "vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap"

# 并行编译任务数；0 表示自动使用 CPU 核心数
JOBS = 0

# 传给 ./emulator.sh 的默认额外参数（run / debug 时追加在产物目录之后）
# 示例: ["-no-window"], ["-keep", "-no-window"]
EMULATOR_EXTRA_ARGS: list[str] = []

# debug 子命令附加的 GDB stub 参数（见 Debugging_Vela_with_Vela_Emulator_zh-cn.md）
DEBUG_EMULATOR_ARGS = ["-qemu", "-S", "-s"]

# =============================================================================

BOARD_COMMANDS = frozenset(
    {
        "build",
        "menuconfig",
        "savedefconfig",
        "clean",
        "fullclean",
        "distclean",
    },
)

RUN_COMMANDS = frozenset({"run", "emulator", "debug"})

COMMANDS = (
    "demo",
    "build",
    "menuconfig",
    "savedefconfig",
    "run",
    "emulator",
    "debug",
    "clean",
    "fullclean",
    "distclean",
    "complete",
    "help",
)

# 互斥的 LVX / AI Demo Kconfig 符号（不含 lvgldemo）
LVX_DEMO_KCONFIG_KEYS = (
    "LVX_USE_DEMO_WHACKMOLE",
    "LVX_USE_DEMO_BREAKOUT",
    "LVX_USE_DEMO_CALCULATOR",
    "LVX_USE_DEMO_MUSIC_PLAYER",
    "LVX_USE_DEMO_PET",
    "LVX_USE_DEMO_RELATIVES_CALCULATOR",
    "LVX_USE_DEMO_SNAKE_GAME",
    "LVX_USE_DEMO_WOODEN_FISH",
    "LVX_USE_DEMO_BANDX",
    "LVX_USE_DEMO_X_TRACK",
    "AI_CONVERSATION_DEMO",
)

DEMO_STATE_FILE = ".vela_emulator_demo"

SCRIPT_PATH = Path(__file__).resolve()
SCRIPT_BASENAME = SCRIPT_PATH.name


@dataclass(frozen=True)
class EmulatorDemo:
    """模拟器 Demo 元数据（编号从 1 起）。"""

    name: str
    title: str
    run_cmd: str
    description: str = ""
    kconfig_enable: tuple[str, ...] = ()
    kconfig_values: tuple[tuple[str, str], ...] = ()
    adb_push: tuple[tuple[str, str], ...] = ()
    doc: str = ""


def demo_catalog() -> list[EmulatorDemo]:
    """返回 Demo 列表（顺序即编号 1..N）。"""
    demos_dir = "apps/packages/demos"
    return [
        EmulatorDemo(
            name="lvgldemo",
            title="LVGL 内置演示",
            run_cmd="lvgldemo",
            description="Widgets / Music / Stress 等，默认固件通常已包含",
            kconfig_enable=("EXAMPLES_LVGLDEMO",),
            doc="docs/zh-cn/quickstart/vscode_plugin_usage.md",
        ),
        EmulatorDemo(
            name="music_player",
            title="音乐播放器",
            run_cmd="music_player &",
            description="需推送 wav 与封面资源到 /data",
            kconfig_enable=("LVX_USE_DEMO_MUSIC_PLAYER",),
            kconfig_values=(("LVX_MUSIC_PLAYER_DATA_ROOT", "/data"),),
            adb_push=((f"{demos_dir}/music_player/res", "/data/"),),
            doc="docs/zh-cn/demo/Music_Player_Example_zh-cn.md",
        ),
        EmulatorDemo(
            name="bandx",
            title="智能手环 Bandx",
            run_cmd="bandx &",
            description="依赖 LVGL Fragment；需推送字体与图片",
            kconfig_enable=(
                "LV_USE_FRAGMENT",
                "LVX_USE_DEMO_BANDX",
            ),
            kconfig_values=(("BANDX_BASE_PATH", "/data"),),
            adb_push=(
                (f"{demos_dir}/bandx/resource/font/assets/*", "/data/font/"),
                # 须推到 /data/image/*.png，不能 push assets 目录（会变成 /data/image/assets/*.png）
                (f"{demos_dir}/bandx/resource/image/assets/*", "/data/image/"),
            ),
            doc="docs/zh-cn/demo/Smart_Band_Example_zh-cn.md",
        ),
        EmulatorDemo(
            name="x_track",
            title="自行车码表 X-TRACK",
            run_cmd="x_track &",
            description="需推送 font / images / track 资源",
            kconfig_enable=("LVX_USE_DEMO_X_TRACK",),
            kconfig_values=(("X_TRACK_RESOURCE_DIR_PATH", "/data"),),
            adb_push=(
                (f"{demos_dir}/x_track/resource/font", "/data"),
                (f"{demos_dir}/x_track/resource/images", "/data"),
                (f"{demos_dir}/x_track/resource/track", "/data"),
            ),
            doc="docs/zh-cn/demo/X_Track_zh-cn.md",
        ),
        EmulatorDemo(
            name="Whackmole",
            title="打地鼠",
            run_cmd="Whackmole &",
            kconfig_enable=("LVX_USE_DEMO_WHACKMOLE",),
            kconfig_values=(("LVX_WHACKMOLE_DATA_ROOT", "/data"),),
        ),
        EmulatorDemo(
            name="calculator",
            title="计算器",
            run_cmd="calculator &",
            kconfig_enable=("LVX_USE_DEMO_CALCULATOR",),
            kconfig_values=(("LVX_CALCULATOR_DATA_ROOT", "/data"),),
        ),
        EmulatorDemo(
            name="breakout",
            title="打砖块",
            run_cmd="breakout &",
            kconfig_enable=("LVX_USE_DEMO_BREAKOUT",),
            kconfig_values=(("LVX_BREAKOUT_DATA_ROOT", "/data"),),
        ),
        EmulatorDemo(
            name="pet",
            title="电子宠物",
            run_cmd="pet &",
            kconfig_enable=("LVX_USE_DEMO_PET",),
            kconfig_values=(("LVX_PET_DATA_ROOT", "/data"),),
        ),
        EmulatorDemo(
            name="snake_game",
            title="贪吃蛇",
            run_cmd="snake_game &",
            kconfig_enable=("LVX_USE_DEMO_SNAKE_GAME",),
            kconfig_values=(("LVX_SNAKE_GAME_DATA_ROOT", "/data"),),
        ),
        EmulatorDemo(
            name="wooden_fish",
            title="木鱼",
            run_cmd="wooden_fish &",
            kconfig_enable=(
                "LVX_USE_DEMO_WOODEN_FISH",
                "LV_FONT_MONTSERRAT_24",
            ),
            kconfig_values=(("LVX_WOODEN_FISH_DATA_ROOT", "/data"),),
        ),
        EmulatorDemo(
            name="rel_cal",
            title="亲戚计算器",
            run_cmd="rel_cal &",
            kconfig_enable=("LVX_USE_DEMO_RELATIVES_CALCULATOR",),
        ),
        EmulatorDemo(
            name="ai_conversation",
            title="AI 对话",
            run_cmd="ai_conversation &",
            description="需网络与 /data 资源，配置项较多",
            kconfig_enable=("AI_CONVERSATION_DEMO",),
            doc="packages/demos/ai_chat/",
        ),
    ]


def demo_by_id(demo_id: int) -> EmulatorDemo:
    catalog = demo_catalog()
    if demo_id < 1 or demo_id > len(catalog):
        raise RuntimeError(
            f"无效 Demo 编号: {demo_id}（可选 1..{len(catalog)}）"
        )
    return catalog[demo_id - 1]


def demo_state_path(out: Path) -> Path:
    return out / DEMO_STATE_FILE


def load_saved_demo_id(out: Path) -> int | None:
    path = demo_state_path(out)
    if not path.is_file():
        return None
    try:
        value = int(path.read_text(encoding="utf-8").strip())
    except ValueError:
        return None
    catalog = demo_catalog()
    if 1 <= value <= len(catalog):
        return value
    return None


def save_demo_id(out: Path, demo_id: int) -> None:
    demo_state_path(out).write_text(f"{demo_id}\n", encoding="utf-8")


def find_openvela_root(start: Path | None = None) -> Path:
    """向上查找包含 build.sh 与 nuttx/ 的 openvela 根目录。"""
    cur = (start or Path.cwd()).resolve()
    for directory in (cur, *cur.parents):
        if (directory / "build.sh").is_file() and (directory / "nuttx").is_dir():
            return directory
    raise RuntimeError(
        "无法定位 openvela 根目录（需存在 build.sh 与 nuttx/）。"
        "请在 openvela 工程内执行本脚本。"
    )


def board_config_path(root: Path) -> Path:
    rel = BOARD_CONFIG.strip().rstrip("/")
    path = (root / rel).resolve()
    if not path.is_dir():
        raise RuntimeError(f"板级配置目录不存在: {path}")
    defconfig = path / "defconfig"
    if not defconfig.is_file():
        raise RuntimeError(f"缺少 defconfig: {defconfig}")
    return path


def cmake_out_dir(root: Path) -> Path:
    """
    与 nuttx/tools/build.sh 中 setup_cmake_binary_dir 规则一致:
    cmake_out/<board_name>_<config_name>
    """
    cfg = board_config_path(root)
    config_name = cfg.name
    board_name = cfg.parent.parent.name
    return root / "cmake_out" / f"{board_name}_{config_name}"


def board_config_arg(root: Path) -> str:
    """传给 build.sh 的路径参数（带尾部 /）。"""
    rel = BOARD_CONFIG.strip().rstrip("/")
    return f"{rel}/"


def emulator_script(root: Path) -> Path:
    script = root / "emulator.sh"
    if not script.is_file():
        raise RuntimeError(
            f"未找到 {script}。请确认已 repo sync，"
            "根目录应存在 emulator.sh → prebuilts/emulator/tools/emulator.sh"
        )
    return script


def job_count(cli_jobs: int | None) -> int:
    if cli_jobs is not None and cli_jobs > 0:
        return cli_jobs
    if JOBS > 0:
        return JOBS
    return os.cpu_count() or 1


def run(
    cmd: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    printable = " ".join(cmd)
    print(f"\n>>> {printable}\n")
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        cmd,
        cwd=cwd,
        env=merged,
        check=check,
        text=True,
    )


def ensure_built(out: Path) -> None:
    """确认 CMake 输出目录已有可加载的编译产物。"""
    if not out.is_dir():
        raise RuntimeError(f"构建目录不存在: {out}，请先执行 build")
    nuttx = out / "nuttx"
    dotconfig = out / ".config"
    if not nuttx.is_file() and not dotconfig.is_file():
        raise RuntimeError(
            f"未找到 {nuttx} 或 {dotconfig}，请先 build。\n"
            f"  示例: {SCRIPT_BASENAME} build"
        )


def kconfig_tweak_bin(root: Path) -> Path:
    machine = platform.machine().lower()
    arch = "aarch64" if machine in ("aarch64", "arm64") else "x86_64"
    path = root / "prebuilts" / "build-tools" / f"linux-{arch}" / "bin" / "kconfig-tweak"
    if not path.is_file():
        raise RuntimeError(f"未找到 kconfig-tweak: {path}")
    return path


def olddefconfig_bin(root: Path) -> Path:
    path = root / "prebuilts" / "tools" / "python" / "bin" / "olddefconfig"
    if not path.is_file():
        raise RuntimeError(f"未找到 olddefconfig: {path}")
    return path


def kconfig_env(root: Path, out: Path) -> dict[str, str]:
    """与 nuttx/cmake/menuconfig.cmake 中 KCONFIG_ENV 一致。"""
    return {
        "KCONFIG_CONFIG": str(out / ".config"),
        "EXTERNALDIR": "dummy",
        "APPSDIR": str(root / "apps"),
        "DRIVERS_PLATFORM_DIR": "dummy",
        "APPSBINDIR": str(out / "apps"),
        "BINDIR": str(out),
    }


def invalidate_cmake_config(out: Path, root: Path) -> None:
    """使 CMake 在下次 build 时根据 .config 重新生成 config.h（同 menuconfig 目标）。"""
    config_h = out / "include" / "nuttx" / "config.h"
    if config_h.is_file():
        config_h.unlink()
    nuttx_cmake = root / "nuttx" / "CMakeLists.txt"
    if nuttx_cmake.is_file():
        nuttx_cmake.touch()


def refresh_dotconfig(root: Path, out: Path) -> None:
    """在 cmake 树中运行 olddefconfig（build.sh 无此 ninja 目标）。"""
    env = os.environ.copy()
    env.update(kconfig_env(root, out))
    run([str(olddefconfig_bin(root))], cwd=root / "nuttx", env=env)
    invalidate_cmake_config(out, root)


def ensure_dotconfig(root: Path, out: Path) -> Path:
    """确保 cmake 输出目录已有 .config。"""
    config = out / ".config"
    if config.is_file():
        return config

    if not out.is_dir():
        print("CMake 输出目录不存在，先执行一次配置…")
        run(["./build.sh", board_config_arg(root), "--cmake"], cwd=root)

    print("尚未生成 .config，正在从 defconfig 初始化…")
    defconfig = board_config_path(root) / "defconfig"
    if not defconfig.is_file():
        raise RuntimeError(f"缺少 defconfig: {defconfig}")
    shutil.copy(defconfig, config)
    refresh_dotconfig(root, out)

    if not config.is_file():
        raise RuntimeError(
            f"初始化 .config 失败: {config}\n"
            f"请先执行: {SCRIPT_BASENAME} build"
        )
    return config


def kconfig_tweak(
    root: Path,
    config: Path,
    *,
    enable: tuple[str, ...] = (),
    disable: tuple[str, ...] = (),
    values: tuple[tuple[str, str], ...] = (),
) -> None:
    tweak = str(kconfig_tweak_bin(root))
    cmd = [tweak, "--file", str(config)]
    for sym in disable:
        cmd.extend(["--disable", sym])
    for sym in enable:
        cmd.extend(["--enable", sym])
    for sym, value in values:
        cmd.extend(["--set-str", sym, value])
    if len(cmd) > 3:
        run(cmd, cwd=root)


def apply_demo_config(root: Path, out: Path, demo: EmulatorDemo) -> None:
    """将 Kconfig 写入 cmake_out/.config 并刷新依赖项。"""
    config = ensure_dotconfig(root, out)
    print(f"配置 Demo: [{demo.name}] {demo.title}")

    kconfig_tweak(
        root,
        config,
        disable=LVX_DEMO_KCONFIG_KEYS,
        enable=demo.kconfig_enable,
        values=demo.kconfig_values,
    )
    refresh_dotconfig(root, out)


def adb_bin() -> str | None:
    return shutil.which("adb")


def push_demo_resources(root: Path, demo: EmulatorDemo, *, wait_device: bool) -> None:
    if not demo.adb_push:
        return
    adb = adb_bin()
    if adb is None:
        print(
            "未找到 adb，请安装 android-tools-adb 后手动推送资源:\n"
            "  sudo apt install android-tools-adb"
        )
        for src, dst in demo.adb_push:
            print(f"  adb push {src} {dst}")
        return

    if wait_device:
        print("等待模拟器 adb 就绪…")
        try:
            run([adb, "wait-for-device"], cwd=root, check=True)
        except subprocess.CalledProcessError:
            print("adb 未连接；模拟器启动后请另开终端执行 push。")
            _print_adb_push_commands(demo)
            return

    for src_rel, dst in demo.adb_push:
        pattern = str(root / src_rel)
        if "*" in src_rel:
            matches = sorted(glob.glob(pattern))
            if not matches:
                print(f"跳过（无匹配文件）: {pattern}")
                continue
            for match in matches:
                cmd = [adb, "push", match, dst]
                try:
                    run(cmd, cwd=root, check=True)
                except subprocess.CalledProcessError:
                    print(f"adb push 失败: {' '.join(cmd)}")
            continue
        src = Path(pattern)
        if not src.exists():
            print(f"跳过（路径不存在）: {src}")
            continue
        cmd = [adb, "push", str(src), dst]
        try:
            run(cmd, cwd=root, check=True)
        except subprocess.CalledProcessError:
            print(f"adb push 失败: {' '.join(cmd)}")


def _print_adb_push_commands(demo: EmulatorDemo) -> None:
    for src, dst in demo.adb_push:
        print(f"  adb push {src} {dst}")


def demo_to_dict(demo_id: int, demo: EmulatorDemo) -> dict[str, Any]:
    item = asdict(demo)
    item["id"] = demo_id
    return item


def print_demo_list(*, as_json: bool = False) -> None:
    catalog = demo_catalog()
    if as_json:
        payload = [demo_to_dict(i, d) for i, d in enumerate(catalog, start=1)]
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return

    print("可用 Demo（build/run 使用 -d <编号>，或交互选择）:\n")
    for demo_id, demo in enumerate(catalog, start=1):
        push = "需 adb push" if demo.adb_push else "无额外资源"
        print(f"  [{demo_id:2d}] {demo.name:<18} {demo.title}")
        print(f"       运行: {demo.run_cmd}")
        print(f"       说明: {demo.description or push}")
        if demo.doc:
            print(f"       文档: {demo.doc}")
        print()

    print("Python 列表（数组）:")
    payload = [demo_to_dict(i, d) for i, d in enumerate(catalog, start=1)]
    print("DEMOS = " + json.dumps(payload, ensure_ascii=False, indent=2))


def cmd_demo(*, as_json: bool = False) -> None:
    print_demo_list(as_json=as_json)


def resolve_demo_id(
    cli_demo: int | None,
    out: Path,
    *,
    interactive: bool,
) -> int | None:
    if cli_demo is not None:
        demo_by_id(cli_demo)
        return cli_demo

    saved = load_saved_demo_id(out)
    if saved is not None and not interactive:
        return saved

    if not interactive or not sys.stdin.isatty():
        return saved

    print_demo_list()
    while True:
        try:
            hint = f"1..{len(demo_catalog())}"
            if saved is not None:
                hint += f"，回车沿用 [{saved}]"
            line = input(f"选择 Demo 编号（{hint}）: ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return saved
        if not line:
            return saved
        try:
            choice = int(line)
        except ValueError:
            print("请输入数字编号。")
            continue
        try:
            demo_by_id(choice)
        except RuntimeError as exc:
            print(exc)
            continue
        return choice


def print_demo_run_hint(demo: EmulatorDemo) -> None:
    bar = "=" * 60
    print(f"\n{bar}")
    print(f"Demo: {demo.title} ({demo.name})")
    print(f"模拟器 NSH 中执行: {demo.run_cmd}")
    if demo.adb_push:
        print("资源推送（模拟器启动后另开终端，或已自动 push）:")
        _print_adb_push_commands(demo)
    print(f"{bar}\n")


def merge_emulator_args(*parts: list[str] | tuple[list[str], ...]) -> list[str]:
    merged: list[str] = []
    for group in parts:
        for arg in group:
            if arg not in merged:
                merged.append(arg)
    return merged


def cmd_build(root: Path, jobs: int, demo: EmulatorDemo | None = None) -> None:
    if demo is not None:
        out = cmake_out_dir(root)
        apply_demo_config(root, out, demo)
    run(
        ["./build.sh", board_config_arg(root), "--cmake", f"-j{jobs}"],
        cwd=root,
    )


def cmd_menuconfig(root: Path) -> None:
    run(
        ["./build.sh", board_config_arg(root), "--cmake", "menuconfig"],
        cwd=root,
    )


def cmd_savedefconfig(root: Path) -> None:
    run(
        ["./build.sh", board_config_arg(root), "--cmake", "savedefconfig"],
        cwd=root,
    )


def cmd_run(
    root: Path,
    out: Path,
    extra_args: list[str],
    *,
    debug: bool = False,
    demo: EmulatorDemo | None = None,
) -> None:
    ensure_built(out)
    if demo is not None:
        print_demo_run_hint(demo)
        if demo.adb_push:
            push_demo_resources(root, demo, wait_device=False)
    emu = emulator_script(root)
    args = merge_emulator_args(
        EMULATOR_EXTRA_ARGS,
        DEBUG_EMULATOR_ARGS if debug else [],
        extra_args,
    )
    cmd = [str(emu), str(out), *args]
    if debug:
        print(
            "GDB 调试: 另开终端执行:\n"
            f"  gdb-multiarch {out / 'nuttx'}\n"
            "  (gdb) target remote localhost:1234\n"
            "  (gdb) c\n"
            "文档: docs/zh-cn/quickstart/emulator/Debugging_Vela_with_Vela_Emulator_zh-cn.md\n"
        )
    run(cmd, cwd=root)


def cmd_clean(root: Path) -> None:
    """仅清理编译产物，保留 .config 与 CMake 缓存。"""
    out = cmake_out_dir(root)
    if not out.is_dir():
        print(f"构建目录不存在，跳过 clean: {out}")
        return
    if not (out / "build.ninja").is_file():
        print(f"未找到 Ninja 构建文件，跳过 clean: {out}")
        return
    run(["cmake", "--build", str(out), "-t", "clean"], cwd=root)


def cmd_distclean(root: Path) -> None:
    """通过 build.sh 删除 CMake 输出目录。"""
    run(
        ["./build.sh", board_config_arg(root), "--cmake", "distclean"],
        cwd=root,
    )


def cmd_fullclean(root: Path) -> None:
    """删除整个 CMake 输出目录，并清理 nuttx 树内残留 .config。"""
    out = cmake_out_dir(root)
    if out.is_dir():
        print(f"删除构建目录: {out}")
        shutil.rmtree(out)

    for name in (".config", ".config.old", ".version"):
        path = root / "nuttx" / name
        if path.exists() or path.is_symlink():
            print(f"删除: {path}")
            path.unlink()

    print("fullclean 完成。")


def _commands_str() -> str:
    return " ".join(COMMANDS)


def completion_bash() -> str:
    cmds = _commands_str()
    script = str(SCRIPT_PATH)
    basename = SCRIPT_BASENAME
    return f'''# bash completion for {basename}
# eval "$(python3 {script} complete bash)"

_vela_emulator_tools_completion() {{
    local cur prev
    COMPREPLY=()
    cur="${{COMP_WORDS[COMP_CWORD]}}"
    prev="${{COMP_WORDS[COMP_CWORD-1]}}"

    local script_idx=-1 i word
    for ((i = 0; i < COMP_CWORD; i++)); do
        word="${{COMP_WORDS[i]}}"
        case "$word" in
            {basename}|./{basename}|*/{basename}|*vela_emulator_tools.py)
                script_idx=$i
                break
                ;;
        esac
    done

    if (( script_idx < 0 )); then
        return 0
    fi

    local cmd_start=$((script_idx + 1))
    local has_cmd=0 pos

    for ((pos = cmd_start; pos < COMP_CWORD; pos++)); do
        case "${{COMP_WORDS[pos]}}" in
            -*) ;;
            demo|build|menuconfig|savedefconfig|run|emulator|debug|clean|fullclean|distclean|complete|help)
                has_cmd=1
                break
                ;;
        esac
    done

    if (( COMP_CWORD == cmd_start )) || [[ "$cur" == -* ]]; then
        if [[ "$cur" == -* ]]; then
            COMPREPLY=( $(compgen -W "-j --jobs -h --help" -- "$cur") )
        elif (( has_cmd == 0 )); then
            COMPREPLY=( $(compgen -W "{cmds}" -- "$cur") )
        fi
        return 0
    fi

    if (( has_cmd == 0 )); then
        COMPREPLY=( $(compgen -W "{cmds}" -- "$cur") )
        return 0
    fi

    case "$prev" in
        complete)
            COMPREPLY=( $(compgen -W "bash zsh install" -- "$cur") )
            ;;
        install)
            COMPREPLY=( $(compgen -W "bash zsh" -- "$cur") )
            ;;
    esac
}}

complete -o bashdefault -o default -F _vela_emulator_tools_completion {script}
complete -o bashdefault -o default -F _vela_emulator_tools_completion ./{basename}
complete -o bashdefault -o default -F _vela_emulator_tools_completion {basename}
'''


def completion_zsh() -> str:
    script = str(SCRIPT_PATH)
    basename = SCRIPT_BASENAME
    return f'''#compdef -P 'python3 *{basename}' {basename} {script}

local -a commands
commands=(
  {" ".join(f'"{c}"' for c in COMMANDS)}
)

_arguments -C \\
  '(-j)--jobs[并行任务数]' \\
  '(-h)--help[帮助]' \\
  '*:command:->cmd'

case $state in
  cmd)
    _describe 'command' commands
    ;;
esac
'''


def cmd_complete(shell: str) -> None:
    if shell == "bash":
        print(completion_bash(), end="")
    elif shell == "zsh":
        print(completion_zsh(), end="")
    else:
        raise RuntimeError(f"不支持的 shell: {shell}，可选: bash, zsh")


def cmd_complete_install(shell: str) -> None:
    script = str(SCRIPT_PATH)
    marker_begin = f"# >>> {SCRIPT_BASENAME} completion >>>"
    marker_end = f"# <<< {SCRIPT_BASENAME} completion <<<"
    block = f"""{marker_begin}
eval "$(python3 {script} complete {shell})"
{marker_end}
"""
    if shell == "bash":
        rcfile = Path.home() / ".bashrc"
    elif shell == "zsh":
        rcfile = Path.home() / ".zshrc"
    else:
        raise RuntimeError(f"不支持的 shell: {shell}")

    text = rcfile.read_text(encoding="utf-8") if rcfile.exists() else ""
    for old_begin in (
        "# >>> vela_sifli_tools.py completion >>>",
        "# >>> build_board.py completion >>>",
    ):
        if old_begin in text:
            old_end = old_begin.replace(">>>", "<<<")
            text = re.sub(
                rf"{re.escape(old_begin)}.*?{re.escape(old_end)}\n?",
                "",
                text,
                flags=re.DOTALL,
            )
            rcfile.write_text(text, encoding="utf-8")
            break

    text = rcfile.read_text(encoding="utf-8") if rcfile.exists() else ""
    if marker_begin in text:
        print(f"补全已存在于 {rcfile}")
        return

    with rcfile.open("a", encoding="utf-8") as fh:
        fh.write("\n" + block)
    print(f"已写入 {rcfile}，请执行: source {rcfile}")


def build_parser() -> argparse.ArgumentParser:
    out_example = "cmake_out/vela_goldfish-arm64-v8a-ap"
    parser = argparse.ArgumentParser(
        description="openvela Vela Emulator 辅助（menuconfig / build / run）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""
板级配置（仅改脚本顶部常量，无 -b 参数）:
  BOARD_CONFIG = {BOARD_CONFIG}
  编译产物目录 ≈ {out_example}

示例:
  %(prog)s menuconfig
  %(prog)s build
  %(prog)s build -j8
  %(prog)s demo
  %(prog)s demo --json
  %(prog)s build -d 2
  %(prog)s run -d 1
  %(prog)s build run -d 3
  %(prog)s debug
  %(prog)s run -- -no-window -keep
  %(prog)s complete install

等效命令（openvela 根目录）:
  ./build.sh {BOARD_CONFIG}/ --cmake -j$(nproc)
  ./emulator.sh {out_example}/
""",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=None,
        metavar="N",
        help="并行编译任务数",
    )
    parser.add_argument(
        "-d",
        "--demo",
        type=int,
        default=None,
        metavar="N",
        help="Demo 编号（见 demo 子命令列表）",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="demo 子命令以 JSON 数组输出",
    )
    parser.add_argument(
        "commands",
        nargs="*",
        metavar="COMMAND",
        help="子命令，可多个: build run ...；run/debug 之后的参数用 -- 分隔传给 emulator.sh",
    )
    return parser


def split_argv(argv: list[str]) -> tuple[list[str], list[str]]:
    """将 '--' 之后的参数作为 emulator 额外参数。"""
    if "--" in argv:
        idx = argv.index("--")
        return argv[:idx], argv[idx + 1 :]
    return argv, []


def dispatch_command(
    name: str,
    *,
    root: Path,
    out: Path,
    jobs: int,
    emulator_args: list[str],
    demo: EmulatorDemo | None = None,
) -> None:
    if name == "demo":
        cmd_demo(as_json=False)
    elif name == "build":
        cmd_build(root, jobs, demo=demo)
    elif name == "menuconfig":
        cmd_menuconfig(root)
    elif name == "savedefconfig":
        cmd_savedefconfig(root)
    elif name in ("run", "emulator"):
        cmd_run(root, out, emulator_args, demo=demo)
    elif name == "debug":
        cmd_run(root, out, emulator_args, debug=True, demo=demo)
    elif name == "clean":
        cmd_clean(root)
    elif name == "distclean":
        cmd_distclean(root)
    elif name == "fullclean":
        cmd_fullclean(root)
    else:
        raise RuntimeError(f"未知命令: {name}")


def main() -> int:
    argv, emu_extra = split_argv(sys.argv[1:])

    if len(argv) >= 2 and argv[0] == "complete":
        target = argv[1]
        if target in ("bash", "zsh"):
            cmd_complete(target)
            return 0
        if target == "install":
            shell = "bash"
            if "--shell" in argv:
                idx = argv.index("--shell")
                if idx + 1 < len(argv):
                    shell = argv[idx + 1]
            cmd_complete_install(shell)
            return 0

    parser = build_parser()

    if not argv:
        parser.print_help()
        return 0
    if len(argv) == 1 and argv[0] in ("help", "-h", "--help"):
        parser.print_help()
        return 0

    args = parser.parse_args(argv)
    raw_commands = args.commands or []

    if not raw_commands:
        parser.print_help()
        return 0

    if raw_commands[0] == "help":
        parser.print_help()
        return 0

    if raw_commands[0] == "complete":
        if len(raw_commands) < 2:
            parser.error("complete 需要参数: bash | zsh | install")
        if raw_commands[1] == "install":
            shell = "bash"
            if "--shell" in argv:
                idx = argv.index("--shell")
                if idx + 1 < len(argv):
                    shell = argv[idx + 1]
            cmd_complete_install(shell)
        else:
            cmd_complete(raw_commands[1])
        return 0

    unknown = [c for c in raw_commands if c not in COMMANDS]
    if unknown:
        parser.error(f"未知命令: {', '.join(unknown)}；可选: {_commands_str()}")

    if args.json and raw_commands != ["demo"]:
        parser.error("--json 仅用于 demo 子命令")

    try:
        jobs = job_count(args.jobs)
        root = find_openvela_root(SCRIPT_PATH.parent)
        board_config_path(root)
        out = cmake_out_dir(root)

        if raw_commands == ["demo"]:
            cmd_demo(as_json=args.json)
            return 0

        needs_demo = bool(
            BOARD_COMMANDS.intersection(raw_commands)
            or RUN_COMMANDS.intersection(raw_commands)
        )
        demo_obj: EmulatorDemo | None = None
        demo_id: int | None = None
        if needs_demo:
            interactive = "build" in raw_commands or bool(
                RUN_COMMANDS.intersection(raw_commands)
            )
            demo_id = resolve_demo_id(
                args.demo,
                out,
                interactive=interactive and args.demo is None,
            )
            if demo_id is not None:
                demo_obj = demo_by_id(demo_id)
                save_demo_id(out, demo_id)
                print(f"已选择 Demo [{demo_id}]: {demo_obj.title} ({demo_obj.name})")

        if BOARD_COMMANDS.intersection(raw_commands) or RUN_COMMANDS.intersection(
            raw_commands
        ):
            print(f"openvela 根目录 : {root}")
            print(f"板级配置       : {BOARD_CONFIG}")
            print(f"CMake 输出目录 : {out}")

        for command in raw_commands:
            dispatch_command(
                command,
                root=root,
                out=out,
                jobs=jobs,
                emulator_args=emu_extra,
                demo=demo_obj,
            )

    except subprocess.CalledProcessError as exc:
        print(f"\n命令失败，退出码: {exc.returncode}", file=sys.stderr)
        return exc.returncode
    except RuntimeError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\n已中断。", file=sys.stderr)
        return 130

    return 0


if __name__ == "__main__":
    sys.exit(main())
