"""List every address currently advertising NUS / Sidewalk_NUS (live packets only)."""
import asyncio

from bleak import BleakScanner

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"


async def main():
    seen = {}

    def cb(dev, adv):
        if adv.local_name == "Sidewalk_NUS" or NUS_SERVICE in adv.service_uuids:
            seen.setdefault(dev.address, []).append(adv.rssi)

    async with BleakScanner(cb):
        await asyncio.sleep(8)

    for addr, rssi in seen.items():
        print(f"{addr} adv packets: {len(rssi)} last rssi: {rssi[-1]}")
    if not seen:
        print("nothing seen")


asyncio.run(main())
