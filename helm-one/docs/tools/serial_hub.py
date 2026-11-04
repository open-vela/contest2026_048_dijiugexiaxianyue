#!/usr/bin/env python3
"""串口转发枢纽（serial hub）：**一个独立小程序**，谁都能用它共享一个串口。

## 模型（2026-09-22 lease 化）

`/tmp` **只放一把信物**：`/tmp/vela-serial-<串口名>-<hash8>.lease`
（hash = sha1(本工具绝对路径 + 串口绝对路径) 前 8 位，避免多份 SDK/工程撞车）。
抢到信物的进程独占 `/dev/tty…`，并用 Linux **abstract** Unix 套接字
（`@vela-serial-<串口名>-<hash8>`，不落盘）做多路复用。日志 spool 在
`~/.cache/vela/serial/<串口名>-<hash8>.log`。

后来的 `monitor` 读信物 → 挂到 abstract 端点。烧录走
`release`（关 TTY、`state=yielded`）→ sftool 独占 → `acquire`+可选 pulse
（重开、`gen++`）。

**它自己会死**：最后一个客户端离开后 `--idle-timeout`（工具拉起默认 **300 s**；
手工起默认 5 s）退出并释放信物。`release` 期间不算 idle。

## 怎么用

```bash
python3 serial_hub.py /dev/ttyACM0 --baud 1000000
python3 serial_hub.py /dev/ttyACM0 --reset
python3 serial_hub.py /dev/ttyACM0 --no-idle-exit

# 客户端经 vela_my_vendor_tools（读 lease → 连 abstract）
python3 vela_my_vendor_tools.py monitor --status -p /dev/ttyACM0
```

## 协议

一行一个 JSON。`attach` 后推 `{"ev":"rx","b64":"…"}`。

| op | 作用 |
|----|------|
| `status` | `{ok, port, baud, open, released, clients, hub_pid, lease, sock, gen}` |
| `rx` / `send` / `tx` / `attach` | 同前 |
| `release` / `acquire` | 让出 / 收回串口（烧录）；`acquire` 可带 `pulse` |
| `pulse` / `shutdown` | 复位脉冲 / 收工 |
"""

from __future__ import annotations

import argparse
import base64
import fcntl
import hashlib
import json
import os
import re
import signal
import socket
import sys
import threading
import time
from collections import deque
from pathlib import Path

RX_RING = 64 * 1024
READ_TIMEOUT_S = 0.2
CLIENT_IO_TIMEOUT_S = 1.0
SERIAL_WRITE_TIMEOUT_S = 1.0
FANOUT_QUEUE_MAX = 64
FANOUT_SEND_TIMEOUT_S = 0.2
IDLE_EXIT_S = 5.0
HANDOVER_MAX_S = 180.0
POLL_MS = 250
IDLE_MS = 250


# ---- 信物 / 端点命名 -------------------------------------------------------

def device_tag(port: str) -> str:
    dev = Path(str(port)).name or "serial"
    return re.sub(r"[^A-Za-z0-9_.-]", "_", dev)


def _hub_run_root() -> str:
    """身份根：本 serial_hub.py 的绝对路径（可用 VELA_SERIAL_HUB_ROOT 覆盖）。"""
    env = os.environ.get("VELA_SERIAL_HUB_ROOT", "").strip()
    if env:
        return str(Path(env).expanduser().resolve())
    return str(Path(__file__).resolve())


def _port_key(port: str) -> str:
    """串口身份：尽量 realpath，节点暂不存在时退回规范化路径。"""
    p = Path(str(port)).expanduser()
    try:
        return str(p.resolve())
    except OSError:
        return str(p)


def instance_id(port: str) -> str:
    """运行路径 + 串口路径 → 短 hash，避免多份工程撞同一 /tmp 信物。"""
    raw = f"{_hub_run_root()}\0{_port_key(port)}"
    return hashlib.sha1(raw.encode("utf-8")).hexdigest()[:8]


def instance_tag(port: str) -> str:
    return f"{device_tag(port)}-{instance_id(port)}"


def lease_path_for(port: str) -> Path:
    env = os.environ.get("VELA_SERIAL_HUB_LEASE", "").strip()
    if env:
        return Path(env).expanduser()
    return Path("/tmp") / f"vela-serial-{instance_tag(port)}.lease"


def abstract_sock_name(port: str) -> str:
    """abstract 名（不含 leading NUL / @）。"""
    env = os.environ.get("VELA_SERIAL_HUB_SOCK", "").strip()
    if env.startswith("@"):
        return env[1:]
    if env.startswith("\0"):
        return env[1:]
    # 旧环境变量若仍是路径，忽略，按实例命名。
    return f"vela-serial-{instance_tag(port)}"


def abstract_sock_display(port: str) -> str:
    return f"@{abstract_sock_name(port)}"


def abstract_sock_addr(port: str) -> str:
    """Python AF_UNIX abstract：首字节为 NUL。"""
    return "\0" + abstract_sock_name(port)


def spool_path_for(port: str) -> Path:
    env = os.environ.get("VELA_MONITOR_SPOOL", "").strip() or os.environ.get(
        "MONITOR_SPOOL", ""
    ).strip()
    if env:
        return Path(env).expanduser()
    base = Path.home() / ".cache" / "vela" / "serial"
    base.mkdir(parents=True, exist_ok=True)
    return base / f"{instance_tag(port)}.log"


def legacy_lease_path_for(port: str) -> Path:
    """旧版仅按设备名的信物，兼容探测。"""
    return Path("/tmp") / f"vela-serial-{device_tag(port)}.lease"


def legacy_sock_path_for(port: str) -> Path:
    """旧版 /tmp/*.sock，仅兼容探测。"""
    return Path("/tmp") / f"vela-serial-{device_tag(port)}.sock"


def unix_connect_target(endpoint: str | Path) -> str:
    """把 lease 里的 `@name` / Path / 普通路径 转成 connect() 参数。"""
    if isinstance(endpoint, Path):
        return str(endpoint)
    s = str(endpoint)
    if s.startswith("@"):
        return "\0" + s[1:]
    if s.startswith("\0"):
        return s
    return s


def read_lease_json(path: Path) -> dict | None:
    try:
        raw = path.read_text(encoding="utf-8")
        obj = json.loads(raw)
        return obj if isinstance(obj, dict) else None
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        return None


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False
    return True


def resolve_endpoint_from_lease(port: str) -> tuple[str, dict] | None:
    """读信物 → (connect target, meta)。持有者已死则返回 None。"""
    for path, meta in iter_live_leases_for_port(port):
        sock = str(meta.get("sock") or abstract_sock_display(port))
        return unix_connect_target(sock), meta
    return None


def iter_live_leases_for_port(port: str) -> list[tuple[Path, dict]]:
    """同一串口上所有仍存活的信物（含旧名 / 其它 hash），按 mtime 新→旧。"""
    want = _port_key(port)
    want_name = device_tag(port)
    found: list[tuple[float, Path, dict]] = []
    seen: set[Path] = set()
    candidates = [lease_path_for(port), legacy_lease_path_for(port)]
    try:
        for p in Path("/tmp").glob("vela-serial-*.lease"):
            candidates.append(p)
    except OSError:
        pass
    for path in candidates:
        try:
            key = path.resolve()
        except OSError:
            key = path
        if key in seen:
            continue
        seen.add(key)
        meta = read_lease_json(path)
        if not meta:
            continue
        meta_port = str(meta.get("port") or "")
        if meta_port:
            if _port_key(meta_port) != want and device_tag(meta_port) != want_name:
                continue
        elif want_name not in path.name:
            continue
        pid = int(meta.get("pid") or 0)
        if not pid_alive(pid):
            continue
        try:
            mtime = path.stat().st_mtime
        except OSError:
            mtime = 0.0
        found.append((mtime, path, meta))
    found.sort(key=lambda t: t[0], reverse=True)
    return [(p, m) for _, p, m in found]


def other_live_hub(port: str, *, self_pid: int | None = None) -> tuple[Path, dict] | None:
    """若另有 hub 已占同一串口，返回其 (lease, meta)。"""
    me = self_pid if self_pid is not None else os.getpid()
    for path, meta in iter_live_leases_for_port(port):
        if int(meta.get("pid") or 0) == me:
            continue
        return path, meta
    return None


class SerialLease:
    """`/tmp` 信物：flock 排他 + JSON 元数据。持有者进程保持 fd 打开。"""

    def __init__(self, port: str):
        self.port = port
        self.path = lease_path_for(port)
        self._fd: int | None = None
        self.gen = 0
        self.sock_display = abstract_sock_display(port)
        self.spool = str(spool_path_for(port))

    @property
    def held(self) -> bool:
        return self._fd is not None

    def acquire(self, *, baud: int) -> bool:
        """非阻塞抢信物。成功则写入 holding 元数据。"""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(
            str(self.path),
            os.O_RDWR | os.O_CREAT,
            0o600,
        )
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(fd)
            return False
        self._fd = fd
        old = read_lease_json(self.path) or {}
        try:
            self.gen = int(old.get("gen") or 0) + 1
        except (TypeError, ValueError):
            self.gen = 1
        self._write(
            {
                "port": self.port,
                "baud": baud,
                "pid": os.getpid(),
                "sock": self.sock_display,
                "gen": self.gen,
                "state": "holding",
                "yield_until": 0,
                "spool": self.spool,
            }
        )
        return True

    def _write(self, payload: dict) -> None:
        if self._fd is None:
            return
        data = (json.dumps(payload, ensure_ascii=False, indent=0) + "\n").encode(
            "utf-8"
        )
        try:
            os.ftruncate(self._fd, 0)
            os.lseek(self._fd, 0, os.SEEK_SET)
            os.write(self._fd, data)
            os.fsync(self._fd)
        except OSError:
            pass

    def refresh(self, *, baud: int, state: str, yield_until: float = 0.0) -> None:
        if not self.held:
            return
        self._write(
            {
                "port": self.port,
                "baud": baud,
                "pid": os.getpid(),
                "sock": self.sock_display,
                "gen": self.gen,
                "state": state,
                "yield_until": yield_until,
                "spool": self.spool,
            }
        )

    def mark_yielded(self, *, baud: int) -> None:
        self.refresh(
            baud=baud,
            state="yielded",
            yield_until=time.time() + HANDOVER_MAX_S,
        )

    def mark_holding(self, *, baud: int, bump_gen: bool = False) -> None:
        if bump_gen:
            self.gen += 1
        self.refresh(baud=baud, state="holding", yield_until=0.0)

    def release_on_exit(self) -> None:
        fd = self._fd
        self._fd = None
        if fd is None:
            return
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        try:
            os.close(fd)
        except OSError:
            pass
        # 若仍是我们的 pid，删信物，避免尸检。
        meta = read_lease_json(self.path)
        if meta and int(meta.get("pid") or 0) == os.getpid():
            try:
                self.path.unlink()
            except OSError:
                pass


def log(msg: str) -> None:
    print(f"serial-hub: {msg}", flush=True)


class StreamFanout:
    """把 RX 帧推给流式客户端，**不阻塞读线程**。"""

    def __init__(self, *, on_drop=None):
        self._lock = threading.Lock()
        self._queues: dict[socket.socket, deque] = {}
        self._wake = threading.Event()
        self._alive = True
        self._on_drop = on_drop
        self._thread = threading.Thread(
            target=self._writer, name="hub-fanout", daemon=True
        )
        self._thread.start()

    def add(self, conn: socket.socket) -> None:
        try:
            conn.settimeout(FANOUT_SEND_TIMEOUT_S)
        except OSError:
            pass
        with self._lock:
            self._queues.setdefault(conn, deque())

    def discard(self, conn: socket.socket) -> None:
        with self._lock:
            self._queues.pop(conn, None)

    def push(self, frame: bytes) -> None:
        if not frame:
            return
        drop: list[socket.socket] = []
        with self._lock:
            for conn, q in self._queues.items():
                if len(q) >= FANOUT_QUEUE_MAX:
                    drop.append(conn)
                    continue
                q.append(frame)
        self._wake.set()
        for conn in drop:
            log("hub: 客户端积压过深，摘掉（可重新 attach）")
            self._drop(conn)

    def stop(self) -> None:
        self._alive = False
        self._wake.set()

    def _drop(self, conn: socket.socket) -> None:
        self.discard(conn)
        if self._on_drop is not None:
            try:
                self._on_drop(conn)
            except Exception:
                pass
        try:
            conn.close()
        except OSError:
            pass

    def _writer(self) -> None:
        while self._alive:
            self._wake.wait(timeout=0.05)
            self._wake.clear()
            while self._alive:
                item = None
                with self._lock:
                    for conn, q in list(self._queues.items()):
                        if q:
                            item = (conn, q.popleft())
                            break
                if item is None:
                    break
                conn, frame = item
                try:
                    conn.sendall(frame)
                except TimeoutError:
                    log("hub: 客户端发送超时，摘掉它（可重新 attach）")
                    self._drop(conn)
                except OSError as exc:
                    log(f"hub: 客户端写失败（{exc}），摘掉它")
                    self._drop(conn)


class Hub:
    def __init__(
        self,
        port: str,
        baud: int,
        lease: SerialLease,
        *,
        idle_exit: bool,
    ):
        self.port = port
        self.baud = baud
        self.lease = lease
        self.idle_exit = idle_exit
        self.idle_timeout_s = IDLE_EXIT_S
        self.ser = None
        self._rx = bytearray()
        self._rx_cv = threading.Condition()
        self._streams: set[socket.socket] = set()
        self._stream_lock = threading.Lock()
        self._srv: socket.socket | None = None
        self._alive = True
        self.released = False
        self.released_at = 0.0
        self._last_client_gone: float | None = None
        self._write_lock = threading.Lock()
        self.last_rx_mono = 0.0
        self.rx_bytes_total = 0
        self._fanout = StreamFanout(on_drop=self._fanout_dropped)
        self.spool = None
        self.sock_display = lease.sock_display

    def open_port(self, *, reset: bool) -> None:
        import serial

        ser = serial.Serial()
        ser.port = self.port
        ser.baudrate = self.baud
        ser.timeout = READ_TIMEOUT_S
        ser.write_timeout = SERIAL_WRITE_TIMEOUT_S
        ser.dtr = False
        ser.rts = False
        ser.exclusive = True  # 禁止第二把 fd（双 hub 抢口会让 flash 失败）
        ser.open()
        if reset:
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False
        self.ser = ser

    def pulse(self) -> None:
        ser = self.ser
        if ser is None or not getattr(ser, "is_open", False):
            raise RuntimeError("串口没开")
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        ser.dtr = False

    def release_port(self) -> bool:
        ser = self.ser
        if ser is None:
            return False
        try:
            if getattr(ser, "is_open", False):
                ser.close()
        except Exception:
            return False
        self.released = True
        self.released_at = time.monotonic()
        self.lease.mark_yielded(baud=self.baud)
        self.note("串口已让出（烧录中），缓冲保留")
        return True

    def acquire_port(self, *, pulse: bool = False) -> bool:
        ser = self.ser
        if ser is None:
            return False
        try:
            ser.dtr = False
            ser.rts = False
            ser.exclusive = True
            ser.open()
            ser.timeout = READ_TIMEOUT_S
            ser.write_timeout = SERIAL_WRITE_TIMEOUT_S
        except Exception as exc:
            self.released = False
            self.lease.mark_holding(baud=self.baud, bump_gen=True)
            self.note(f"收回串口失败（{exc}），交给重连逻辑")
            return False
        self.released = False
        self.lease.mark_holding(baud=self.baud, bump_gen=True)
        if pulse:
            try:
                self.pulse()
                self.note("已补发复位脉冲，接着抓开机日志（从 SFBL 起）")
            except Exception as exc:
                self.note(f"复位失败（{exc}）")
        return True

    def feed(self, data: bytes) -> None:
        spool = self.spool
        if spool is not None:
            try:
                spool.write(data)
            except OSError:
                pass
        with self._rx_cv:
            self._rx.extend(data)
            extra = len(self._rx) - RX_RING
            if extra > 0:
                del self._rx[:extra]
            self._rx_cv.notify_all()
        self.last_rx_mono = time.monotonic()
        self.rx_bytes_total += len(data)
        self.push(data)

    def rx_snapshot(self) -> bytes:
        with self._rx_cv:
            return bytes(self._rx)

    def note(self, text: str) -> None:
        line = f"\r\n*** serial-hub: {text} ***\r\n".encode("utf-8")
        self.push(line)
        try:
            sys.stdout.write(line.decode("utf-8"))
            sys.stdout.flush()
        except Exception:
            pass

    def write_port(self, payload: bytes) -> int:
        ser = self.ser
        if self.released:
            raise RuntimeError("串口已让出（烧录中），等 acquire")
        if ser is None or not getattr(ser, "is_open", False):
            raise RuntimeError("串口未打开")
        with self._write_lock:
            n = ser.write(payload)
            try:
                ser.flush()
            except Exception:
                pass
        return len(payload) if n is None else int(n)

    def push(self, data: bytes) -> None:
        if not data:
            return
        frame = (
            json.dumps(
                {"ev": "rx", "b64": base64.b64encode(data).decode("ascii")},
                ensure_ascii=False,
            )
            + "\n"
        ).encode("utf-8")
        self._fanout.push(frame)

    def _fanout_dropped(self, conn: socket.socket) -> None:
        with self._stream_lock:
            self._streams.discard(conn)
            left = len(self._streams)
        if left == 0:
            self._last_client_gone = time.monotonic()

    def register(self, conn: socket.socket) -> int:
        with self._stream_lock:
            self._streams.add(conn)
            n = len(self._streams)
        self._fanout.add(conn)
        return n

    def unregister(self, conn: socket.socket) -> None:
        self._fanout.discard(conn)
        with self._stream_lock:
            self._streams.discard(conn)
            left = len(self._streams)
        if left == 0:
            self._last_client_gone = time.monotonic()
        try:
            conn.close()
        except OSError:
            pass

    def clients(self) -> int:
        with self._stream_lock:
            return len(self._streams)

    def serve(self) -> None:
        # spool
        try:
            spool_p = Path(self.lease.spool)
            spool_p.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(
                str(spool_p),
                os.O_WRONLY | os.O_CREAT | os.O_APPEND,
                0o600,
            )
            self.spool = os.fdopen(fd, "ab", buffering=0)
        except OSError:
            self.spool = None

        addr = abstract_sock_addr(self.port)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            # 若上次异常退出留下同名 abstract，bind 会 EADDRINUSE —— 先尝试连一下，
            # 连不上再忽略（abstract 随进程消失，一般可直接 bind）。
            srv.bind(addr)
        except OSError as exc:
            srv.close()
            raise RuntimeError(
                f"abstract 端点已被占用: {self.sock_display} ({exc})；"
                f"检查是否已有 hub（lease={self.lease.path}）"
            ) from exc

        srv.listen(16)
        srv.settimeout(0.5)
        self._srv = srv
        self.lease.mark_holding(baud=self.baud)
        log(
            f"{self.port} @ {self.baud}  lease={self.lease.path}  "
            f"mux={self.sock_display}  gen={self.lease.gen}  pid={os.getpid()}"
        )

        threading.Thread(target=self._read_loop, name="hub-rx", daemon=True).start()
        threading.Thread(target=self._housekeeping, name="hub-idle", daemon=True).start()

        while self._alive:
            try:
                conn, _ = srv.accept()
            except TimeoutError:
                continue
            except OSError:
                break
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def shutdown(self) -> None:
        if not self._alive:
            return
        self._alive = False
        try:
            self._fanout.stop()
        except Exception:
            pass
        log("收工")
        try:
            if self._srv is not None:
                self._srv.close()
        except OSError:
            pass
        ser = self.ser
        try:
            if ser is not None and getattr(ser, "is_open", False):
                ser.close()
        except Exception:
            pass
        spool = self.spool
        if spool is not None:
            try:
                spool.close()
            except OSError:
                pass
        self.lease.release_on_exit()

    def _housekeeping(self) -> None:
        while self._alive:
            time.sleep(POLL_MS / 1000.0)
            if self.released and (time.monotonic() - self.released_at) > HANDOVER_MAX_S:
                log("让出超过上限没人收回，自动收回")
                self.acquire_port()
            if (
                self.idle_exit
                and not self.released
                and self.clients() == 0
                and self._last_client_gone is not None
                and (time.monotonic() - self._last_client_gone) >= self.idle_timeout_s
            ):
                self.note(
                    f"最后一个客户端已离开 {self.idle_timeout_s:g} s，hub 自己退出"
                )
                self.shutdown()
                return

    def _read_loop(self) -> None:
        import serial as serial_mod

        while self._alive:
            if self.released:
                time.sleep(0.05)
                continue
            ser = self.ser
            if ser is None:
                time.sleep(0.2)
                continue
            try:
                data = ser.read(ser.in_waiting or 1)
            except serial_mod.SerialException:
                if self.released:
                    continue
                try:
                    if ser.is_open:
                        ser.close()
                except Exception:
                    pass
                self.note("串口断开，等待重新接入…")
                while self._alive:
                    time.sleep(0.5)
                    try:
                        ser.dtr = False
                        ser.rts = False
                        ser.open()
                    except Exception:
                        continue
                    self.note("串口已恢复")
                    break
                continue
            except Exception:
                time.sleep(0.05)
                continue
            if data:
                self.feed(data)

    def _client(self, conn: socket.socket) -> None:
        conn.settimeout(CLIENT_IO_TIMEOUT_S)
        streaming = False

        def reply(obj: dict) -> bool:
            try:
                conn.sendall(
                    (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
                )
                return True
            except OSError:
                return False

        try:
            buf = b""
            while self._alive:
                try:
                    chunk = conn.recv(65536)
                except TimeoutError:
                    continue
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    if not raw.strip():
                        continue
                    try:
                        req = json.loads(raw.decode("utf-8"))
                        resp = self.handle(req, conn)
                    except Exception as exc:  # noqa: BLE001
                        resp = {"ok": False, "error": str(exc)}
                    if resp is None:
                        continue
                    if not reply(resp):
                        break
                    if resp.get("_shutdown"):
                        self.shutdown()
                        return
                    if resp.get("_stream"):
                        streaming = True
        except OSError:
            pass
        finally:
            if streaming:
                self.unregister(conn)
            else:
                try:
                    conn.close()
                except OSError:
                    pass

    def handle(self, req: dict, conn: socket.socket) -> dict | None:
        op = str(req.get("op") or req.get("cmd") or "send").lower()

        if op in ("ping", "status"):
            rx_age_ms = (
                int((time.monotonic() - self.last_rx_mono) * 1000)
                if self.last_rx_mono
                else -1
            )
            return {
                "ok": True,
                "port": self.port,
                "baud": self.baud,
                "open": self.ser is not None and getattr(self.ser, "is_open", False),
                "released": self.released,
                "clients": self.clients(),
                "hub_pid": os.getpid(),
                "lease": str(self.lease.path),
                "sock": self.sock_display,
                "gen": self.lease.gen,
                "rx_age_ms": rx_age_ms,
                "rx_bytes": self.rx_bytes_total,
            }
        if op == "rx":
            return {"ok": True, "rx": self.rx_snapshot().decode("utf-8", "replace")}
        if op == "pulse":
            try:
                self.pulse()
            except Exception as exc:
                return {"ok": False, "error": f"复位失败: {exc}"}
            return {"ok": True}
        if op == "release":
            return {"ok": self.release_port(), "released": self.released}
        if op == "acquire":
            return {
                "ok": self.acquire_port(pulse=bool(req.get("pulse"))),
                "released": self.released,
                "gen": self.lease.gen,
            }
        if op == "shutdown":
            self.note("收到 shutdown")
            return {"ok": True, "_shutdown": True}
        if op == "tx":
            payload = base64.b64decode(str(req.get("b64") or ""))
            self.write_port(payload)
            return {"ok": True, "len": len(payload)}
        if op == "attach":
            replay = int(req.get("replay") or 0)
            if replay > 0:
                history = self.rx_snapshot()[-replay:]
                if history:
                    try:
                        conn.sendall(
                            (
                                json.dumps(
                                    {
                                        "ev": "rx",
                                        "b64": base64.b64encode(history).decode(
                                            "ascii"
                                        ),
                                        "replay": len(history),
                                    },
                                    ensure_ascii=False,
                                )
                                + "\n"
                            ).encode("utf-8")
                        )
                    except OSError:
                        return {"ok": False, "error": "回放失败"}
            n = self.register(conn)
            return {
                "ok": True,
                "port": self.port,
                "baud": self.baud,
                "clients": n,
                "gen": self.lease.gen,
                "_stream": True,
            }
        if op != "send":
            return {"ok": False, "error": f"unknown op: {op}"}

        hexdata = req.get("hex")
        if hexdata:
            payload = bytes.fromhex(str(hexdata).replace(" ", ""))
        else:
            payload = str(req.get("text") or req.get("data") or "").encode("utf-8")
            if req.get("nl", True) and payload and not payload.endswith((b"\n", b"\r")):
                payload += b"\n"

        wait_s = max(int(req.get("wait_ms", 2000)), 0) / 1000.0
        idle_s = max(int(req.get("idle_ms", IDLE_MS)), 1) / 1000.0
        with self._rx_cv:
            mark = len(self._rx)
        self.write_port(payload)
        deadline = time.monotonic() + wait_s
        last_len = mark
        last_change = time.monotonic()
        with self._rx_cv:
            while True:
                now = time.monotonic()
                if now >= deadline:
                    break
                self._rx_cv.wait(timeout=min(idle_s, max(deadline - now, 0.01)))
                n = len(self._rx)
                if n != last_len:
                    last_len = n
                    last_change = now
                    continue
                if n > mark and (time.monotonic() - last_change) >= idle_s:
                    break
            return {
                "ok": True,
                "rx": bytes(self._rx[mark:]).decode("utf-8", "replace"),
            }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="串口转发枢纽：lease 信物 + abstract 多路复用",
    )
    parser.add_argument("port", help="串口设备，如 /dev/ttyACM0")
    parser.add_argument("-b", "--baud", type=int, default=1000000)
    parser.add_argument(
        "--sock",
        default=None,
        help="兼容旧参数：若给 @name 则覆盖 abstract 名；文件系统路径已忽略",
    )
    parser.add_argument("--lease", default=None, help="信物路径（默认 /tmp/vela-serial-<dev>-<hash8>.lease）")
    parser.add_argument("--reset", action="store_true", help="起来先补一次硬件复位")
    parser.add_argument("--no-reset", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--no-idle-exit", action="store_true", help="没有客户端也不退出")
    parser.add_argument("--idle-timeout", type=float, default=IDLE_EXIT_S)
    args = parser.parse_args(argv)
    if args.no_reset:
        args.reset = False

    port = args.port
    if args.lease:
        os.environ["VELA_SERIAL_HUB_LEASE"] = str(Path(args.lease).expanduser())
    if args.sock:
        s = str(args.sock)
        if s.startswith("@") or s.startswith("\0"):
            os.environ["VELA_SERIAL_HUB_SOCK"] = s
        elif s.endswith(".sock"):
            log(f"忽略旧文件系统 sock 参数（已改 abstract）: {s}")

    lease = SerialLease(port)
    if not lease.acquire(baud=args.baud):
        meta = read_lease_json(lease.path) or {}
        print(
            f"serial-hub: 信物已被占用: {lease.path} "
            f"(pid={meta.get('pid')} sock={meta.get('sock')})",
            file=sys.stderr,
        )
        return 1

    # 改名/换 hash 后可能残留旧 hub：它仍占着 TTY，本进程再开会抢口。
    peer = other_live_hub(port, self_pid=os.getpid())
    if peer is not None:
        peer_path, peer_meta = peer
        lease.release_on_exit()
        print(
            f"serial-hub: 同串口已有其它 hub：lease={peer_path} "
            f"pid={peer_meta.get('pid')} sock={peer_meta.get('sock')}；"
            f"请先 `monitor stop -p {port}` 或结束该进程，勿并行起两份",
            file=sys.stderr,
        )
        return 1

    hub = Hub(port, args.baud, lease, idle_exit=not args.no_idle_exit)
    hub.idle_timeout_s = float(args.idle_timeout)


    def _term(_signum, _frame):
        raise KeyboardInterrupt

    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        try:
            signal.signal(sig, _term)
        except (ValueError, OSError):
            pass

    try:
        hub.open_port(reset=args.reset)
        hub.serve()
    except KeyboardInterrupt:
        pass
    except Exception as exc:  # noqa: BLE001
        print(f"serial-hub: 启动失败: {exc}", file=sys.stderr)
        lease.release_on_exit()
        return 1
    finally:
        hub.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
