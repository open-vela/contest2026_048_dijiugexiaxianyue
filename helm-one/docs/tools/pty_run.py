#!/usr/bin/env python3
"""在分配出来的伪终端（pty）里跑一个命令，输出同时写进日志文件。

## 为什么需要它

`monitor` 子命令走的是 pyserial 的 `miniterm`，而 `miniterm` 的 `Console`
**必须有真 tty**：它启动时 `termios.tcgetattr(fd)` 读终端属性，stdin 是管道
或重定向（`nohup ... &`、`cmd > log &`、CI、agent 的后台任务）时会直接抛

    termios.error: (25, 'Inappropriate ioctl for device')

于是"后台起一个 monitor、用 monitor-ctl 另一边读日志"这件事做不成。本脚本
给子进程一个 pty 当作控制台，把它看到的一切按原样写进日志文件 —— 串口数据、
崩溃解码的彩色输出、monitor 自己打的 `*** monitor: ... ***` 标记，全都在里面。

## 用法

```bash
# 后台起 monitor，日志写 /tmp/mon.log，控制套接字照常可用
python3 vendor/my_vendor/docs/tools/pty_run.py /tmp/mon.log \
    python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py -p /dev/ttyACM0 monitor &

# 另一终端照旧
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor-ctl --rx
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor-ctl --status
```

日志文件是**追加**写（便于断点续看）；前台运行时也能看到同样的内容。

## 注意

- 子进程收到 SIGINT/SIGTERM 时会被一起带走，不会留下孤儿 monitor 占着串口。
- 退出码是子进程的退出码（被信号杀死时是 128+信号）。
- 只为"没有 tty 也要跑 tty 程序"这件事存在；有真终端时直接跑 monitor 即可。
"""

from __future__ import annotations

import argparse
import os
import pty
import signal
import sys
import time

# macOS/BSD 与 Linux 通用；Linux 上 pty.fork 自己会把子进程放进新会话。
_CHILD_EXIT_POLL_S = 0.2


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="在 pty 里跑命令并把输出写进日志文件（给后台 monitor 用）",
        usage="pty_run.py <logfile> <command> [args...]",
    )
    parser.add_argument("logfile", help="输出追加写入的日志文件")
    parser.add_argument("cmd", nargs=argparse.REMAINDER, help="要运行的命令")
    args = parser.parse_args(argv)

    cmd = list(args.cmd)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        parser.error("缺少要运行的命令")

    log_path = os.path.abspath(os.path.expanduser(args.logfile))
    pid, fd = pty.fork()
    if pid == 0:
        # 子进程：pty 从设备已经是 stdin/stdout/stderr（pty.fork 的约定）。
        try:
            os.execvp(cmd[0], cmd)
        except OSError as exc:
            os.write(2, f"pty_run: 执行 {cmd[0]!r} 失败: {exc}\n".encode())
            os._exit(127)

    exit_code = 0
    stopping = False

    def _forward(signum, _frame):
        nonlocal stopping
        stopping = True
        try:
            os.kill(pid, signum)
        except OSError:
            pass

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        try:
            signal.signal(sig, _forward)
        except (ValueError, OSError):
            pass

    with open(log_path, "ab", buffering=0) as log:
        while True:
            try:
                data = os.read(fd, 65536)
            except OSError:
                # pty 从设备关闭（子进程退出/关闭了控制台）：
                # Linux 上这里通常是 EIO，没有更多输出可读。
                break
            if not data:
                break
            log.write(data)
            if not stopping:
                try:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                except (BrokenPipeError, ValueError):
                    pass

    try:
        _, status = os.waitpid(pid, 0)
        if os.WIFEXITED(status):
            exit_code = os.WEXITSTATUS(status)
        elif os.WIFSIGNALED(status):
            exit_code = 128 + os.WTERMSIG(status)
    except ChildProcessError:
        pass

    # 子进程可能刚退出、pty 里还有尾巴；稍等一下再收尾，避免日志缺最后几行。
    time.sleep(_CHILD_EXIT_POLL_S)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
