# SPDX-License-Identifier: Apache-2.0
#
# Host regression test for the BLE supervisor escalation contract.
# No NuttX toolchain needed; run from this directory or with -C.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -pthread
INCLUDES = -I.. -I../../../include
TARGET   = ble_supervisor_test

.PHONY: all run clean

all: $(TARGET)

$(TARGET): ble_supervisor_test.c ../ble_supervisor.c ../ble_supervisor.h ../../../include/myvendor_ble_log.h
	$(CC) $(CFLAGS) $(INCLUDES) ble_supervisor_test.c ../ble_supervisor.c -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
