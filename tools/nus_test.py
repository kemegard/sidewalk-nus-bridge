"""Host-side check of the sidewalk_nus bridge using the PC Bluetooth adapter.

python nus_test.py --uart COM152 [--name Sidewalk_NUS] [--provisioned]

1. UART line   -> NUS notification, mirrored as "[uart->nus] ..."
2. NUS input   -> !help/!status/!mtu/!ping answered; anything else answered with the command list.
                  Input mirrored as "[nus] ...", replies as "[cmd] ..."
3. Sidewalk (AMA) and SMP characteristics must be rejected on the NUS link (authorization)
4. After disconnect the device must advertise again
"""
import argparse
import asyncio
import sys
import time

import serial
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify
AMA_WRITE = "74f996c9-7d6c-4d58-9232-0427ab61c53c"
SMP_CHAR = "da2e7828-fbce-4e01-ae9e-261174997c48"

results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" - {detail}" if detail else ""))


def read_uart_until(port, needle, timeout=3.0):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        buf += port.read(256)
        if needle in buf:
            return True, buf
    return False, buf


async def find(name, timeout=15.0):
    return await BleakScanner.find_device_by_name(name, timeout=timeout)


async def main(args):
    uart = serial.Serial(args.uart, 115200, timeout=0.1)
    uart.reset_input_buffer()

    dev = await find(args.name)
    check("advertising found", dev is not None, dev.address if dev else "not found")
    if dev is None:
        return

    notifications = []

    async with BleakClient(dev, timeout=20.0, winrt={"use_cached_services": False}) as client:
        check("connected", client.is_connected)
        await client.start_notify(NUS_TX, lambda _, data: notifications.append(bytes(data)))

        # 1. UART -> NUS
        line = b"ping from uart"
        uart.write(line + b"\r\n")
        end = time.time() + 3
        while time.time() < end and line not in notifications:
            await asyncio.sleep(0.1)
        check("UART line notified over NUS", line in notifications, repr(notifications))
        ok, buf = read_uart_until(uart, b"[uart->nus] " + line)
        check("UART line mirrored on NUS UART", ok)

        # 2. NUS input: only commands are accepted, anything else gets the command list
        ignored = b"ignored, allowed commands: !status !mtu !ping !help"
        expected = {
            b"!help": b"commands: !status !mtu !ping !help",
            b"!status": b"status:" if args.provisioned else b"status: sidewalk not started",
            b"!mtu": b"mtu:" if args.provisioned else b"mtu: sidewalk not started",
            b"!ping": b"ping:" if args.provisioned else b"ping: sidewalk not started",
            b"!bogus": ignored,
            b"hello from pc": ignored,
            b"": ignored,
        }
        for cmd, reply in expected.items():
            label = repr(cmd.decode())
            notifications.clear()
            uart.reset_input_buffer()
            await client.write_gatt_char(NUS_RX, cmd + b"\r\n", response=True)
            end = time.time() + 3
            while time.time() < end and not any(n.startswith(reply) for n in notifications):
                await asyncio.sleep(0.1)
            got = b"".join(notifications)
            check(f"{label} replied over NUS", any(n.startswith(reply) for n in notifications),
                  got.decode(errors="replace"))
            ok, buf = read_uart_until(uart, b"[cmd] " + reply, timeout=1.0)
            check(f"{label} mirrored on NUS UART", ok and b"[nus] " + cmd in buf)

        # 3. Sidewalk / SMP attributes are blocked on the NUS link
        for label, uuid in (("Sidewalk AMA write", AMA_WRITE), ("SMP", SMP_CHAR)):
            if client.services.get_characteristic(uuid) is None:
                check(f"{label} not reachable on NUS link", True, "characteristic not present")
                continue
            try:
                await client.write_gatt_char(uuid, b"\x00", response=True)
                check(f"{label} rejected on NUS link", False, "write accepted")
            except (BleakError, OSError) as e:
                check(f"{label} rejected on NUS link", True, str(e).splitlines()[0])

        await client.stop_notify(NUS_TX)

    # 4. advertising restarts after disconnect
    await asyncio.sleep(1.0)
    dev2 = await find(args.name)
    check("advertising restarted after disconnect", dev2 is not None)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--uart", required=True, help="COM port of the NUS UART (uart30)")
    p.add_argument("--name", default="Sidewalk_NUS")
    p.add_argument("--provisioned", action="store_true",
                   help="device has Sidewalk credentials (expect real !status/!mtu/!ping replies)")
    a = p.parse_args()
    asyncio.run(main(a))
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    sys.exit(1 if failed else 0)
