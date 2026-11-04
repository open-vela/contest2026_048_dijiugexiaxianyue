#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
openvela SiFli 板级构建辅助脚本（用法类似 ESP-IDF / sf_sdk_tools.py）。

文档:
  vendor/sifli/docs/sf32lb52_build_guide.md

用法（在 openvela 根目录或任意子目录执行）:
  python3 vela_sifli_tools.py menuconfig
  python3 vela_sifli_tools.py build
  python3 vela_sifli_tools.py flash
  python3 vela_sifli_tools.py monitor
  python3 vela_sifli_tools.py build flash monitor    # 可组合，顺序执行

Tab 补全（bash）:
  eval "$(python3 vela_sifli_tools.py complete bash)"
  python3 vela_sifli_tools.py complete install
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

# =============================================================================
# 工程配置 — 切换板子时主要改这里
# =============================================================================

# 相对 openvela 根目录的板级 config 路径（末尾可有可无 /）
BOARD_CONFIG = "vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh"

# UART 下载 / 串口监视（Linux: /dev/ttyUSB0；Windows: COM19 或 19）
PORT: str | None = None

# 串口监视波特率（SF32 日志口常用 1000000，与 CONFIG_UART_BAUD 一致）
MONITOR_BAUD = 1000000

# NuttX readline 仅以 LF(\\n) 作为行结束（见 readline_common.c），Enter 须发 \\n 不能发 \\r
# NSH 由设备 readline 回显，保持 False；bttool 用 getline 无回显，由 MONITOR_SMART_BTTOOL 自动开本地 echo
MONITOR_ECHO = False
MONITOR_EOL = "lf"  # NuttX NSH 用 lf；RT-Thread MSH 可试 cr
# bttool 下自动开本地回显（getline 无设备回显）；空回车原样转发给设备
MONITOR_SMART_BTTOOL = True
# True：串口收到的 VT100/ANSI（清行 [K、颜色等）原样交给终端，否则会显示成 ␛[K
MONITOR_RAW = True
# miniterm 过滤器：direct=透传；default 会把 ESC 转成可见乱码
MONITOR_FILTER = "direct"
# NSH Tab 补全（须 defconfig 含 CONFIG_READLINE_TABCOMPLETION=y 并重新 build）
# menuconfig: System Libraries → readline() → Tab completion
# ttyACM/USB-CDC 须拉高 DTR，否则主机发不出数据
MONITOR_DTR = True
MONITOR_RTS = True

# 并行编译任务数；0 表示自动使用 CPU 核心数
JOBS = 0

# sftool 烧录（固件为 cmake_out/<board>_<config>/nuttx.bin）
FLASH_CHIP = "SF32LB52"
FLASH_MEMORY = "nor"
FLASH_ADDRESS = "0x12010000"
# 可选额外镜像，格式 path@address（路径相对 CMake 输出目录或绝对路径）
# 例如与 SiFli SDK 一并烧录 ftab: "ftab/ftab.bin@0x12000000"
FLASH_EXTRAS: list[str] = []

# =============================================================================

BOARD_COMMANDS = frozenset(
    {
        "build",
        "menuconfig",
        "savedefconfig",
        "flash",
        "clean",
        "fullclean",
        "distclean",
    },
)

COMMANDS = (
    "build",
    "menuconfig",
    "savedefconfig",
    "flash",
    "monitor",
    "clean",
    "fullclean",
    "distclean",
    "complete",
    "help",
)

SCRIPT_PATH = Path(__file__).resolve()
SCRIPT_BASENAME = SCRIPT_PATH.name


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


def tab_completion_status(root: Path) -> bool | None:
    """检查 Tab 补全是否已在 defconfig 或已构建的 .config 中启用。"""
    cfg = board_config_path(root)
    defconfig = (cfg / "defconfig").read_text(encoding="utf-8", errors="replace")
    if "CONFIG_READLINE_TABCOMPLETION=y" in defconfig:
        return True

    dotconfig = cmake_out_dir(root) / ".config"
    if dotconfig.is_file():
        text = dotconfig.read_text(encoding="utf-8", errors="replace")
        if "CONFIG_READLINE_TABCOMPLETION=y" in text:
            return True
        if "# CONFIG_READLINE_TABCOMPLETION is not set" in text:
            return False

    if (
        "CONFIG_READLINE_TABCOMPLETION is not set" in defconfig
        or "# CONFIG_READLINE_TABCOMPLETION is not set" in defconfig
    ):
        return False
    return None


def warn_tab_completion(root: Path) -> None:
    """monitor/build 前提示 Tab 补全配置状态。"""
    status = tab_completion_status(root)
    if status is True:
        return
    if status is False:
        print(
            "警告: 未启用 CONFIG_READLINE_TABCOMPLETION，NSH Tab 补全无效。\n"
            "  1) 在 defconfig 加入: CONFIG_READLINE_TABCOMPLETION=y\n"
            "  2) 或: ./vela_sifli_tools.py menuconfig\n"
            "     → System Libraries and NSH → readline() → Tab completion\n"
            "  3) build 后执行 savedefconfig 写回 defconfig\n",
            file=sys.stderr,
        )
        return
    print(
        "提示: 若 Tab 补全无效，请确认 CONFIG_READLINE_TABCOMPLETION=y 并已重新 build。",
        file=sys.stderr,
    )


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


def list_serial_ports() -> list[str]:
    system = platform.system()
    ports: list[str] = []
    if system == "Darwin":
        dev = Path("/dev")
        for entry in dev.iterdir():
            name = entry.name
            if name.startswith("cu.") and (
                "usb" in name.lower() or "serial" in name.lower() or "SLAB" in name
            ):
                ports.append(str(entry))
        ports.sort()
    elif system == "Linux":
        for pattern in ("ttyUSB*", "ttyACM*"):
            ports.extend(str(p) for p in Path("/dev").glob(pattern))
        ports.sort()
    return ports


def resolve_port(port: str | None) -> str:
    if port:
        port = port.strip()
        if port.isdigit() and platform.system() == "Windows":
            return f"COM{port}"
        return port

    ports = list_serial_ports()
    if not ports:
        raise RuntimeError(
            "未指定串口且未自动检测到设备。请使用 -p/--port，"
            "例如: -p /dev/ttyUSB0 或 -p COM19"
        )

    print("可用串口:")
    for idx, name in enumerate(ports):
        print(f"  [{idx}] {name}")

    sel = input("输入序号或直接输入端口名: ").strip()
    if sel.isdigit():
        index = int(sel)
        if 0 <= index < len(ports):
            return ports[index]
        raise RuntimeError(f"无效序号: {sel}")
    if sel:
        return sel
    raise RuntimeError("未选择串口")


def _resolve_flash_path(out: Path, spec: str) -> str:
    """将 path@addr 中的 path 解析为绝对路径（若在 out 下存在则相对 out）。"""
    if "@" not in spec:
        raise RuntimeError(f"FLASH 参数格式应为 path@address: {spec}")
    path_part, addr = spec.rsplit("@", 1)
    path = Path(path_part)
    if not path.is_absolute():
        candidate = out / path
        if candidate.is_file():
            path = candidate
    return f"{path}@{addr}"


def load_sftool_flash_args(out: Path) -> tuple[str, str, list[str]]:
    """从 sftool_param.json、uart_download.sh 或默认 nuttx.bin 解析烧录参数。"""
    param_file = out / "sftool_param.json"
    if param_file.is_file():
        data = json.loads(param_file.read_text(encoding="utf-8"))
        chip = data["chip"]
        memory = data["memory"].lower()
        args: list[str] = []
        for item in data.get("write_flash", {}).get("files", []):
            path = item["path"]
            if not Path(path).is_absolute():
                path = str((out / path).resolve())
            if "address" in item:
                args.append(f"{path}@{item['address']}")
            else:
                args.append(path)
        if args:
            return chip, memory, args

    sh = out / "uart_download.sh"
    if sh.is_file():
        text = sh.read_text(encoding="utf-8")
        match = re.search(
            r'sftool\s+-p\s+"\$input"\s+-c\s+(\S+)\s+-m\s+(\S+)\s+write_flash\s+(.+)$',
            text,
            re.MULTILINE,
        )
        if match:
            chip, memory, tail = match.group(1), match.group(2), match.group(3).strip()
            resolved: list[str] = []
            for token in tail.split():
                token = token.strip('"')
                if "@" in token and not Path(token.split("@", 1)[0]).is_absolute():
                    resolved.append(_resolve_flash_path(out, token))
                else:
                    resolved.append(token)
            return chip, memory, resolved

    firmware = out / "nuttx.bin"
    if not firmware.is_file():
        raise RuntimeError(
            f"未找到固件 {firmware}。请先 build，或在 {out} 放置 sftool_param.json。"
        )

    args = [f"{firmware}@{FLASH_ADDRESS}"]
    for extra in FLASH_EXTRAS:
        args.append(_resolve_flash_path(out, extra))
    return FLASH_CHIP, FLASH_MEMORY.lower(), args


def find_sftool() -> str:
    """查找 sftool：PATH → SIFLI_SDK_TOOLS_PATH → ~/.sifli/tools/sftool。"""
    exe = shutil.which("sftool")
    if exe:
        return exe

    tools_roots: list[Path] = []
    env_tools = os.environ.get("SIFLI_SDK_TOOLS_PATH")
    if env_tools:
        tools_roots.append(Path(env_tools))
    tools_roots.append(Path.home() / ".sifli")

    candidates: list[Path] = []
    for root in tools_roots:
        sftool_dir = root / "tools" / "sftool"
        if not sftool_dir.is_dir():
            continue
        for ver_dir in sftool_dir.iterdir():
            candidate = ver_dir / "sftool"
            if candidate.is_file():
                candidates.append(candidate)

    if candidates:
        candidates.sort(key=lambda p: p.parent.name, reverse=True)
        return str(candidates[0].resolve())

    raise RuntimeError(
        "未找到 sftool。请安装 SiFli SDK 工具链: "
        "cd $SIFLI_SDK && ./install.sh && source export.sh；"
        "或将 ~/.sifli/tools/sftool/*/sftool 加入 PATH。"
    )


def cmd_build(root: Path, jobs: int) -> None:
    warn_tab_completion(root)
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


def cmd_flash(out: Path, port: str | None) -> None:
    if not out.is_dir():
        raise RuntimeError(f"构建目录不存在: {out}，请先 build")

    resolved = resolve_port(port)
    chip, memory, flash_args = load_sftool_flash_args(out)
    sftool = find_sftool()

    print(
        "提示: 建议在开始下载前复位开发板，复位后 2 秒内开始烧录。"
        f"\n固件: {out / 'nuttx.bin'}"
        f"\n（等效于 {out.name}/uart_download.sh -p {resolved}）"
    )
    cmd = [
        sftool,
        "-p",
        resolved,
        "-c",
        chip,
        "-m",
        memory,
        "write_flash",
        *flash_args,
    ]
    run(cmd, cwd=out)


_MONITOR_EOL_MAP = {"cr": "CR", "lf": "LF", "crlf": "CRLF"}
_MONITOR_EOL_MINITERM = {"cr": "cr", "lf": "lf", "crlf": "crlf"}


def _run_miniterm_subprocess(
    resolved: str,
    baud: int,
    *,
    echo: bool,
    eol: str,
) -> None:
    """退回 pyserial 自带 miniterm CLI（无 bttool 智能处理）。"""
    eol_arg = _MONITOR_EOL_MAP.get(eol.lower())
    if eol_arg is None:
        raise RuntimeError(f"无效 MONITOR_EOL={eol!r}，可选: cr | lf | crlf")

    args = [
        sys.executable,
        "-m",
        "serial.tools.miniterm",
        resolved,
        str(baud),
        "--eol",
        eol_arg,
        "--dtr",
        "1" if MONITOR_DTR else "0",
        "--rts",
        "1" if MONITOR_RTS else "0",
    ]
    if MONITOR_RAW:
        args.append("--raw")
    if MONITOR_FILTER:
        args.extend(["-f", MONITOR_FILTER])
    if echo:
        args.append("-e")
    subprocess.run(args, check=True)


def _run_sifli_miniterm(resolved: str, baud: int, *, smart_bttool: bool) -> None:
    """SiFli monitor：NSH 设备回显；bttool 自动本地回显，按键原样转发。"""
    import serial
    from serial.tools.miniterm import Miniterm

    eol = _MONITOR_EOL_MINITERM.get(MONITOR_EOL.lower())
    if eol is None:
        raise RuntimeError(
            f"无效 MONITOR_EOL={MONITOR_EOL!r}，可选: cr | lf | crlf"
        )

    filters: tuple[str, ...] = ()
    if MONITOR_FILTER:
        filters = (MONITOR_FILTER,)

    ser = serial.serial_for_url(resolved, baud, do_not_open=True)
    if not hasattr(ser, "cancel_read"):
        ser.timeout = 1
    ser.dtr = MONITOR_DTR
    ser.rts = MONITOR_RTS
    ser.open()

    class SifliMiniterm(Miniterm):
        """见 nsh>/bttool> 切换本地 echo；bttool 用 getline 需本地回显。"""

        _RX_TAIL_MAX = 512

        def __init__(self, *args, smart_bttool: bool = True, **kwargs):
            super().__init__(*args, **kwargs)
            self.smart_bttool = smart_bttool
            self._rx_tail = b""

        def _on_rx_bytes(self, data: bytes) -> None:
            if not self.smart_bttool:
                return
            self._rx_tail = (self._rx_tail + data)[-self._RX_TAIL_MAX :]
            if b"nsh>" in self._rx_tail:
                self.echo = False
            if b"bttool>" in self._rx_tail:
                self.echo = True

        def reader(self):
            import serial as serial_mod

            try:
                while self.alive and self._reader_alive:
                    data = self.serial.read(self.serial.in_waiting or 1)
                    if data:
                        self._on_rx_bytes(data)
                        if self.raw:
                            self.console.write_bytes(data)
                        else:
                            text = self.rx_decoder.decode(data)
                            for transformation in self.rx_transformations:
                                text = transformation.rx(text)
                            self.console.write(text)
            except serial_mod.SerialException:
                self.alive = False
                self.console.cancel()
                raise

    term = SifliMiniterm(
        ser,
        echo=MONITOR_ECHO,
        eol=eol,
        filters=filters,
        smart_bttool=smart_bttool,
    )
    term.raw = MONITOR_RAW
    # 与 miniterm CLI 一致，初始化编解码器（否则 writer 无 tx_encoder）
    term.set_rx_encoding("UTF-8")
    term.set_tx_encoding("UTF-8")
    term.start()
    try:
        term.join()
    finally:
        term.stop()
        term.join(transmit_only=True)
        term.close()


def cmd_monitor(
    port: str | None,
    baud: int,
    *,
    root: Path | None = None,
    smart_bttool: bool | None = None,
) -> None:
    resolved = resolve_port(port)
    if root is not None:
        warn_tab_completion(root)
    try:
        import serial  # noqa: F401
    except ImportError as exc:
        raise RuntimeError(
            "monitor 需要 pyserial: pip install pyserial"
        ) from exc

    if smart_bttool is None:
        smart_bttool = MONITOR_SMART_BTTOOL

    print(
        f"串口监视: {resolved} @ {baud} bps "
        f"(dtr={MONITOR_DTR}, rts={MONITOR_RTS}, echo={MONITOR_ECHO}, "
        f"eol={MONITOR_EOL}, raw={MONITOR_RAW}, filter={MONITOR_FILTER}, "
        f"smart_bttool={smart_bttool}，Ctrl+] 退出)"
    )
    print(
        "提示: NSH 用设备回显；bttool 自动开本地回显，回车原样转发。"
        "若不需要可加 --no-smart-bttool，进 bttool 后 Ctrl+T 再按 e 开 echo。"
    )
    try:
        if smart_bttool:
            _run_sifli_miniterm(resolved, baud, smart_bttool=True)
        else:
            _run_miniterm_subprocess(
                resolved, baud, echo=MONITOR_ECHO, eol=MONITOR_EOL
            )
    except KeyboardInterrupt:
        pass


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

_vela_sifli_tools_completion() {{
    local cur prev
    COMPREPLY=()
    cur="${{COMP_WORDS[COMP_CWORD]}}"
    prev="${{COMP_WORDS[COMP_CWORD-1]}}"

    local script_idx=-1 i word
    for ((i = 0; i < COMP_CWORD; i++)); do
        word="${{COMP_WORDS[i]}}"
        case "$word" in
            {basename}|./{basename}|*/{basename}|*vela_sifli_tools.py|*build_board.py)
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
            build|menuconfig|savedefconfig|flash|monitor|clean|fullclean|distclean|complete|help)
                has_cmd=1
                break
                ;;
        esac
    done

    if (( COMP_CWORD == cmd_start )) || [[ "$cur" == -* ]]; then
        if [[ "$cur" == -* ]]; then
            COMPREPLY=( $(compgen -W "-p --port -j --jobs -b -B --baud -h --help" -- "$cur") )
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

complete -o bashdefault -o default -F _vela_sifli_tools_completion {script}
complete -o bashdefault -o default -F _vela_sifli_tools_completion ./{basename}
complete -o bashdefault -o default -F _vela_sifli_tools_completion {basename}
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
  '(-p)--port[串口]' \\
  '(-j)--jobs[并行任务数]' \\
  '(-b -B)--baud[监视波特率]' \\
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
        "# >>> build_board.py completion >>>",
        "# >>> sf_build_board.py completion >>>",
        "# >>> sf_sdk_tools.py completion >>>",
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
    parser = argparse.ArgumentParser(
        description="openvela SiFli 板级辅助（menuconfig / build / flash / monitor）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""
板级配置（仅改脚本顶部常量，无 -b 参数）:
  BOARD_CONFIG = {BOARD_CONFIG}
  PORT         = {PORT or '(自动检测)'}
  MONITOR_BAUD = {MONITOR_BAUD}
  FLASH        = nuttx.bin @ {FLASH_ADDRESS}

示例:
  %(prog)s menuconfig
  %(prog)s build
  %(prog)s build -j8
  %(prog)s flash -p /dev/ttyUSB0
  %(prog)s monitor -b 1000000
  %(prog)s build flash monitor -p /dev/ttyACM0 -b 1500000
  %(prog)s build flash monitor -p /dev/ttyACM0
  %(prog)s savedefconfig
  %(prog)s complete install

NSH Tab 补全（需已编入固件）:
  CONFIG_NSH_READLINE=y + CONFIG_READLINE_TABCOMPLETION=y
  改 defconfig 后须 build；monitor 时按 Tab（发送 0x09）
""",
    )
    parser.add_argument(
        "-p",
        "--port",
        default=None,
        help=f"串口（默认: 顶部 PORT 或自动检测；当前 PORT={PORT!r}）",
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
        "-b",
        "-B",
        "--baud",
        type=int,
        default=MONITOR_BAUD,
        metavar="RATE",
        dest="baud",
        help=f"monitor 串口波特率，-b 与 -B 等价（默认 MONITOR_BAUD={MONITOR_BAUD}）",
    )
    parser.add_argument(
        "--no-smart-bttool",
        action="store_true",
        help="monitor 禁用 bttool 智能回显（使用原始 miniterm）",
    )
    parser.add_argument(
        "commands",
        nargs="*",
        metavar="COMMAND",
        help="子命令，可多个: build flash monitor ...",
    )
    return parser


def dispatch_command(
    name: str,
    *,
    root: Path,
    out: Path,
    jobs: int,
    port: str | None,
    baud: int,
    smart_bttool: bool = True,
) -> None:
    if name == "build":
        cmd_build(root, jobs)
    elif name == "menuconfig":
        cmd_menuconfig(root)
    elif name == "savedefconfig":
        cmd_savedefconfig(root)
    elif name == "flash":
        cmd_flash(out, port)
        time.sleep(0.3)
    elif name == "monitor":
        cmd_monitor(port, baud, root=root, smart_bttool=smart_bttool)
    elif name == "clean":
        cmd_clean(root)
    elif name == "distclean":
        cmd_distclean(root)
    elif name == "fullclean":
        cmd_fullclean(root)
    else:
        raise RuntimeError(f"未知命令: {name}")


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "complete":
        target = sys.argv[2]
        if target in ("bash", "zsh"):
            cmd_complete(target)
            return 0
        if target == "install":
            shell = "bash"
            if "--shell" in sys.argv:
                idx = sys.argv.index("--shell")
                if idx + 1 < len(sys.argv):
                    shell = sys.argv[idx + 1]
            cmd_complete_install(shell)
            return 0

    parser = build_parser()

    if len(sys.argv) == 1:
        parser.print_help()
        return 0
    if len(sys.argv) == 2 and sys.argv[1] in ("help", "-h", "--help"):
        parser.print_help()
        return 0

    args = parser.parse_args()
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
            if "--shell" in sys.argv:
                idx = sys.argv.index("--shell")
                if idx + 1 < len(sys.argv):
                    shell = sys.argv[idx + 1]
            cmd_complete_install(shell)
        else:
            cmd_complete(raw_commands[1])
        return 0

    unknown = [c for c in raw_commands if c not in COMMANDS]
    if unknown:
        parser.error(f"未知命令: {', '.join(unknown)}；可选: {_commands_str()}")

    try:
        port = args.port if args.port is not None else PORT
        baud = args.baud
        jobs = job_count(args.jobs)
        smart_bttool = not args.no_smart_bttool

        if raw_commands == ["monitor"]:
            try:
                root = find_openvela_root(SCRIPT_PATH.parent)
                warn_tab_completion(root)
            except RuntimeError:
                pass
            cmd_monitor(port, baud, smart_bttool=smart_bttool)
            return 0

        root = find_openvela_root(SCRIPT_PATH.parent)
        board_config_path(root)
        out = cmake_out_dir(root)

        if BOARD_COMMANDS.intersection(raw_commands):
            print(f"openvela 根目录 : {root}")
            print(f"板级配置       : {BOARD_CONFIG}")
            print(f"CMake 输出目录 : {out}")

        for command in raw_commands:
            dispatch_command(
                command,
                root=root,
                out=out,
                jobs=jobs,
                port=port,
                baud=baud,
                smart_bttool=smart_bttool,
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
