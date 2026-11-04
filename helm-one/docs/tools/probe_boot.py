"""探针：开串口 → 复位脉冲 → 读 3 秒，看板子能不能起来。

用来区分"板子没起来"和"monitor 的收回+脉冲没生效"。
用法：先 `monitor-ctl release`，跑它，再 `monitor-ctl acquire`。
"""
import sys
import time

import serial

ser = serial.Serial()
ser.port = "/dev/ttyACM0"
ser.baudrate = 1000000
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()
time.sleep(0.3)
print("--- 复位脉冲 ---")
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
t0 = time.time()
buf = b""
while time.time() - t0 < 3.0:
    buf += ser.read(4096)
print(f"脉冲后 3 秒收到 {len(buf)} 字节")
sys.stdout.write(buf[:400].decode("utf-8", "replace"))
sys.stdout.write("\n...\n")
sys.stdout.write(buf[-400:].decode("utf-8", "replace"))
ser.close()
