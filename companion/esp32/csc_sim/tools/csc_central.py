#!/usr/bin/env python3
"""Reference CSC central on the bench host (BlueZ over D-Bus).

It plays exactly the role the Helm One bike computer plays: discover a CSC
sensor, connect, subscribe to the CSC Measurement characteristic, and derive
cadence from the notification stream with the same arithmetic
(rpm = revs_delta * 1024 * 60 / time_delta, event times in 1/1024 s).

Usage:
    python3 csc_central.py                      # find + connect + 15 s of cadence
    python3 csc_central.py -s 30 --addr C0:DE:CA:DE:00:01
    python3 csc_central.py --cp-test            # also poke the SC Control Point
"""
import argparse
import sys
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

BLUEZ = "org.bluez"
OM_IFACE = "org.freedesktop.DBus.ObjectManager"
PROP_IFACE = "org.freedesktop.DBus.Properties"
DEV_IFACE = "org.bluez.Device1"
ADAPTER_IFACE = "org.bluez.Adapter1"
CHR_IFACE = "org.bluez.GattCharacteristic1"

UUID_CSC_MEAS = "00002a5b-0000-1000-8000-00805f9b34fb"
UUID_CSC_FEAT = "00002a5c-0000-1000-8000-00805f9b34fb"
UUID_CSC_LOC = "00002a5d-0000-1000-8000-00805f9b34fb"
UUID_CSC_CP = "00002a55-0000-1000-8000-00805f9b34fb"
UUID_BAT_LEVEL = "00002a19-0000-1000-8000-00805f9b34fb"
UUID_BAS = "0000180f-0000-1000-8000-00805f9b34fb"
UUID_CSC_SVC = "00001816-0000-1000-8000-00805f9b34fb"


def objects(bus):
    return bus.call_blocking(BLUEZ, "/", OM_IFACE, "GetManagedObjects", "", [])


def find_adapter(objs):
    for path, ifaces in objs.items():
        if ADAPTER_IFACE in ifaces:
            return path
    raise SystemExit("no bluetooth adapter on this host")


def find_device(objs, addr):
    for path, ifaces in objs.items():
        if DEV_IFACE in ifaces and str(ifaces[DEV_IFACE].get("Address", "")).upper() == addr.upper():
            return path
    return None


def wait_for_device(bus, adapter, addr, timeout=15.0, force_scan=False):
    dev = None if force_scan else find_device(objects(bus), addr)
    if dev is not None:
        return dev

    print("scanning for %s ..." % addr)
    bus.call_blocking(BLUEZ, adapter, ADAPTER_IFACE, "StartDiscovery", "", [])
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            time.sleep(0.5)
            dev = find_device(objects(bus), addr)
            if dev is not None:
                return dev
    finally:
        try:
            bus.call_blocking(BLUEZ, adapter, ADAPTER_IFACE, "StopDiscovery", "", [])
        except dbus.exceptions.DBusException:
            pass
    raise SystemExit("device %s not found" % addr)


def prop(bus, path, iface, name):
    return bus.call_blocking(BLUEZ, path, PROP_IFACE, "Get", "", [iface, name])


def connect_device(bus, adapter, addr):
    """Connect and return the device path once the link is up.

    Deliberately does not wait for the ServicesResolved property: bluetoothd on
    this host is slow and erratic about publishing it, while the GATT objects
    show up regardless - so discovery is left to wait_for_gatt() below."""
    dev_path = wait_for_device(bus, adapter, addr)
    dev = bus.get_object(BLUEZ, dev_path)
    dbus.Interface(dev, PROP_IFACE).Set(DEV_IFACE, "Trusted", dbus.Boolean(True))

    for attempt in (1, 2):
        try:
            dbus.Interface(dev, DEV_IFACE).Connect()
            break
        except dbus.exceptions.DBusException as exc:
            # A cached device connects to nothing if the sensor rebooted since it
            # was last seen: drop the stale entry by discovering it again.
            print("connect attempt %d failed: %s" % (attempt, exc.get_dbus_message()))
            if attempt == 2:
                raise SystemExit("could not connect to %s" % addr)
            wait_for_device(bus, adapter, addr, timeout=12.0, force_scan=True)
            time.sleep(1.0)

    started = time.time()
    deadline = started + 20.0
    while time.time() < deadline:
        dev_path = find_device(objects(bus), addr)
        if dev_path is not None:
            try:
                if bool(prop(bus, dev_path, DEV_IFACE, "Connected")):
                    return dev_path
            except dbus.exceptions.DBusException:
                pass
        time.sleep(0.2)

    raise SystemExit("link to %s did not come up" % addr)


def wait_for_gatt(bus, addr, timeout=75.0):
    """Poll the D-Bus tree until the CSC service and its characteristics appear.

    Returns (services_by_uuid, chars_by_uuid) where each value is
    (object_path, properties)."""
    started = time.time()
    deadline = started + timeout
    seen = 0

    while time.time() < deadline:
        objs = objects(bus)
        dev_path = find_device(objs, addr)
        services, chars = {}, {}
        if dev_path is not None:
            for path, ifaces in objs.items():
                if not path.startswith(dev_path + "/"):
                    continue
                if "org.bluez.GattService1" in ifaces:
                    services[str(ifaces["org.bluez.GattService1"].get("UUID", "")).lower()] = path
                if CHR_IFACE in ifaces:
                    chars[str(ifaces[CHR_IFACE].get("UUID", "")).lower()] = (path, ifaces[CHR_IFACE])

        if len(chars) != seen:
            print("  +%.1fs gatt: %d services, %d characteristics" %
                  (time.time() - started, len(services), len(chars)))
            seen = len(chars)

        if UUID_CSC_MEAS in chars:
            return services, chars

        time.sleep(0.5)

    raise SystemExit("GATT database was not published in time")


def children(objs, dev_path, iface):
    """Yield (path, props) of the device's objects exposing iface."""
    for path, ifaces in objs.items():
        if path.startswith(dev_path + "/") and iface in ifaces:
            yield path, ifaces[iface]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-a", "--addr", default="C0:DE:CA:DE:00:01")
    ap.add_argument("-s", "--seconds", type=float, default=15.0)
    ap.add_argument("--cp-test", action="store_true",
                    help="write the SC Control Point to check its error handling")
    ap.add_argument("--keep", action="store_true", help="stay connected at the end")
    args = ap.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    objs = objects(bus)
    adapter = find_adapter(objs)
    print("connect ...")
    dev_path = connect_device(bus, adapter, args.addr)
    print("device path %s" % dev_path)

    print("waiting for the GATT database ...")
    services, chars = wait_for_gatt(bus, args.addr)
    if UUID_CSC_SVC not in services:
        raise SystemExit("no CSC service on the device!")
    print("CSC service found: %s" % services[UUID_CSC_SVC])

    for uuid, label in ((UUID_CSC_FEAT, "feature"), (UUID_CSC_LOC, "location"),
                        (UUID_BAT_LEVEL, "battery")):
        if uuid not in chars:
            print("%-9s: (absent)" % label)
            continue
        path = chars[uuid][0]
        try:
            value = bytes(bus.call_blocking(BLUEZ, path, CHR_IFACE, "ReadValue", "", [{}]))
        except dbus.exceptions.DBusException as exc:
            print("%-9s: read failed: %s" % (label, exc.get_dbus_name()))
            continue
        if uuid == UUID_CSC_FEAT:
            feat = int.from_bytes(value, "little")
            print("%-9s: 0x%04x  wheel=%d crank=%d multi-location=%d" %
                  (label, feat, feat & 1, (feat >> 1) & 1, (feat >> 2) & 1))
        else:
            print("%-9s: %s" % (label, value.hex()))

    if UUID_CSC_MEAS not in chars:
        raise SystemExit("no CSC Measurement characteristic!")
    meas_path, meas_props = chars[UUID_CSC_MEAS]
    print("measurement: %s props=%s" % (meas_path, ",".join(meas_props.get("Flags", []))))

    if args.cp_test and UUID_CSC_CP in chars:
        # No indication subscription: the profile says the sensor must answer
        # with "CCC improperly configured" (0x81).
        try:
            bus.call_blocking(BLUEZ, chars[UUID_CSC_CP][0], CHR_IFACE, "WriteValue", "",
                              [dbus.Array([dbus.Byte(0x01), dbus.Byte(0), dbus.Byte(0),
                                           dbus.Byte(0), dbus.Byte(0)], signature="y"), {}])
            print("control point: write accepted (expected an error)")
        except dbus.exceptions.DBusException as exc:
            print("control point: write rejected with %s (CCC improper)" %
                  exc.get_dbus_message().split(":", 1)[-1].strip())

    state = {"revs": None, "time": None, "packets": 0, "rpms": []}

    def on_props(iface, changed, _invalidated, path=None):
        if iface != CHR_IFACE or path != meas_path or "Value" not in changed:
            return
        value = bytes(changed["Value"])
        state["packets"] += 1
        if len(value) < 5:
            print("packet too short: %s" % value.hex())
            return
        flags = value[0]
        off = 1
        wheel = None
        if flags & 0x01:
            wheel = (int.from_bytes(value[1:5], "little"), int.from_bytes(value[5:7], "little"))
            off += 6
        if not flags & 0x02:
            print("flags=0x%02x wheel=%s  (no crank data)" % (flags, wheel))
            return
        revs = int.from_bytes(value[off:off + 2], "little")
        evt = int.from_bytes(value[off + 2:off + 4], "little")
        rpm = None
        if state["revs"] is not None:
            drev = (revs - state["revs"]) & 0xffff
            dtime = (evt - state["time"]) & 0xffff
            if drev and dtime:
                rpm = drev * 1024 * 60 / dtime
                state["rpms"].append(rpm)
        state["revs"], state["time"] = revs, evt
        print("#%-3d flags=0x%02x revs=%-5d evt=%-5d  -> %s rpm" %
              (state["packets"], flags, revs, evt,
               ("%.1f" % rpm) if rpm is not None else "(baseline)"))

    bus.add_signal_receiver(on_props, signal_name="PropertiesChanged",
                            dbus_interface=PROP_IFACE, path_keyword="path")

    print("subscribing to notifications for %.0f s ..." % args.seconds)
    try:
        bus.call_blocking(BLUEZ, meas_path, CHR_IFACE, "StartNotify", "", [])

        loop = GLib.MainLoop()
        GLib.timeout_add(int(args.seconds * 1000), lambda: (loop.quit(), False)[1])
        try:
            loop.run()
        except KeyboardInterrupt:
            pass

        try:
            bus.call_blocking(BLUEZ, meas_path, CHR_IFACE, "StopNotify", "", [])
        except dbus.exceptions.DBusException:
            pass
    finally:
        if not args.keep:
            # Always drop the link: a CSC sensor stops advertising while linked,
            # so a link left open makes the sensor invisible to everything else.
            path = find_device(objects(bus), args.addr) or dev_path
            try:
                bus.call_blocking(BLUEZ, path, DEV_IFACE, "Disconnect", "", [])
            except dbus.exceptions.DBusException as exc:
                print("disconnect failed: %s" % exc.get_dbus_message())

    rpms = state["rpms"]
    print("\n%d notifications, %d cadence samples" % (state["packets"], len(rpms)))
    if rpms:
        print("cadence: min %.1f  max %.1f  mean %.1f rpm" %
              (min(rpms), max(rpms), sum(rpms) / len(rpms)))

    return 0 if rpms else 1


if __name__ == "__main__":
    sys.exit(main())
