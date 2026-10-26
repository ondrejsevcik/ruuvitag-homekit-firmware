#!/usr/bin/env python3
"""
End-to-end HomeKit test: aiohomekit (the HomeKit controller used by Home Assistant)
pairs with and talks to the simulated RuuviTag (tests/sim, built as tests/host/_build/hk_sim).

The accessory side runs the firmware's HomeKit code; only the SoftDevice, flash
storage and Ruuvi drivers are simulated. The controller side is aiohomekit with its
Bluetooth client replaced by a pipe to the simulator.

    python3 -m venv /tmp/hkvenv && /tmp/hkvenv/bin/pip install aiohomekit==4.0.1
    make -C tests/host _build/hk_sim
    /tmp/hkvenv/bin/python tests/sim/hap_e2e.py [path/to/hk_sim]
"""

from __future__ import annotations

import asyncio
import logging
import os
import subprocess
import sys
from dataclasses import dataclass, field

from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

import aiohomekit.controller.ble.discovery as ble_discovery
import aiohomekit.controller.ble.pairing as ble_pairing
from aiohomekit.characteristic_cache import CharacteristicCacheMemory
from aiohomekit.controller.ble.controller import BleController
from aiohomekit.model.characteristics import CharacteristicsTypes
from aiohomekit.model.services import ServicesTypes

from aiohomekit.crypto import srp as aiohomekit_srp

# aiohomekit bug workaround: the HomeKit ADK (like real accessories) sends the SRP public key B
# without leading zero bytes (HAPPairingPairSetup.c, "Skip leading zeros"). aiohomekit hashes it
# unpadded, so about 1 in 256 pairings fails with "AuthenticationError: step 4". iOS pads it.
_set_server_public_key = aiohomekit_srp.SrpClient.set_server_public_key


def _set_padded_server_public_key(self, B_b):
    _set_server_public_key(self, aiohomekit_srp.pad_left(bytes(B_b), aiohomekit_srp.HK_KEY_LENGTH))


aiohomekit_srp.SrpClient.set_server_public_key = _set_padded_server_public_key

HERE = os.path.dirname(os.path.abspath(__file__))
SIM_BINARY = os.path.join(HERE, "..", "host", "_build", "hk_sim")
# Apple's public HomeKit ADK test vector, also used by the tests/host provisioning.
SETUP_CODE = "518-08-582"
CLIENT_MTU = int(os.environ.get("HK_MTU", "185"))

log = logging.getLogger("hap_e2e")


# ---------------------------------------------------------------------------------------------------------------------
# Simulator process.


class Simulator:
    def __init__(self, binary: str) -> None:
        env = dict(os.environ)
        self.proc = subprocess.Popen(
            [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1, env=env
        )
        self.events: list[str] = []
        line = self._readline()
        assert line == "READY", f"simulator failed to start: {line!r}"

    def _readline(self) -> str:
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(f"simulator exited (code {self.proc.poll()})")
        return line.rstrip("\n")

    def command(self, cmd: str) -> tuple[list[str], str]:
        """Sends a command, returns (output lines, OK line). EVT lines are queued."""
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        out = []
        while True:
            line = self._readline()
            if line.startswith("EVT "):
                self.events.append(line[4:])
            elif line.startswith("OK"):
                return out, line
            else:
                out.append(line)

    def status(self, cmd: str) -> list[str]:
        out, ok = self.command(cmd)
        parts = ok.split()
        if len(parts) < 2 or parts[1] != "0":
            raise RuntimeError(f"{cmd!r} failed: {ok}")
        return parts[2:] if len(parts) > 2 else out

    def take_events(self) -> list[str]:
        events, self.events = self.events, []
        return events

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.stdin.write("QUIT\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=10)


def parse_ad(data: bytes) -> dict[int, bytes]:
    fields = {}
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            break
        fields[data[i + 1]] = data[i + 2 : i + 1 + n]
        i += 1 + n
    return fields


@dataclass
class Advertisement:
    active: bool
    interval_ms: float
    address: str
    manufacturer_data: dict[int, bytes]
    name: str | None


def read_advertisement(sim: Simulator) -> Advertisement:
    out, _ = sim.command("ADV")
    _, active, interval, addr, adv, scan, *_ = out[0].split(" ")
    fields = parse_ad(bytes.fromhex(adv))
    if scan != "-":
        fields.update(parse_ad(bytes.fromhex(scan)))
    mfr = {}
    if 0xFF in fields:
        mfr[int.from_bytes(fields[0xFF][:2], "little")] = fields[0xFF][2:]
    name = fields.get(0x09, fields.get(0x08, b"")).decode() or None
    address = ":".join(addr[i : i + 2] for i in range(0, 12, 2)).upper()
    return Advertisement(active == "1", int(interval) * 0.625, address, mfr, name)


# ---------------------------------------------------------------------------------------------------------------------
# Minimal bleak client replacement.


@dataclass(eq=False)
class FakeDescriptor:
    uuid: str
    handle: int


@dataclass(eq=False)
class FakeCharacteristic:
    uuid: str
    handle: int  # value handle
    properties: list[str]
    service_uuid: str
    descriptors: list[FakeDescriptor] = field(default_factory=list)
    max_write_without_response_size: int = 0

    def get_descriptor(self, uuid: str) -> FakeDescriptor | None:
        for d in self.descriptors:
            if d.uuid.lower() == str(uuid).lower():
                return d
        return None

    def __hash__(self) -> int:
        return self.handle


@dataclass(eq=False)
class FakeService:
    uuid: str
    handle: int
    characteristics: list[FakeCharacteristic] = field(default_factory=list)

    def get_characteristic(self, uuid) -> FakeCharacteristic | None:
        for c in self.characteristics:
            if c.uuid.lower() == str(uuid).lower():
                return c
        return None


class FakeServices:
    def __init__(self, services: list[FakeService]) -> None:
        self.services = {s.handle: s for s in services}
        self.characteristics = {c.handle: c for s in services for c in s.characteristics}

    def __iter__(self):
        return iter(self.services.values())


def load_database(sim: Simulator) -> FakeServices:
    out, _ = sim.command("DB")
    services: list[FakeService] = []
    for line in out:
        parts = line.split()
        if parts[1] == "S":
            services.append(FakeService(parts[3], int(parts[2])))
        elif parts[1] == "C":
            bits = parts[5]
            props = [p for p, b in zip(["read", "write-without-response", "write", "notify", "indicate"], bits) if b == "1"]
            services[-1].characteristics.append(FakeCharacteristic(parts[4], int(parts[3]), props, services[-1].uuid))
        elif parts[1] == "D":
            uuid = parts[3]
            if len(uuid) == 4:
                uuid = f"0000{uuid}-0000-1000-8000-00805f9b34fb"
            services[-1].characteristics[-1].descriptors.append(FakeDescriptor(uuid, int(parts[2])))
    return FakeServices(services)


class FakeBleakClient(ble_pairing.AIOHomeKitBleakClient):
    """Implements the subset of AIOHomeKitBleakClient used by aiohomekit on top of the simulator."""

    connections = 0

    def __init__(self, sim: Simulator, address: str, disconnected_callback) -> None:  # noqa: D107
        # Deliberately not calling BleakClient.__init__ (no real Bluetooth backend).
        self.sim = sim
        self._address = address
        self._disconnected_callback = disconnected_callback
        self._char_cache = {}
        self._iid_cache = {}
        self._notify: dict[int, object] = {}
        self._connected = False

    async def connect_sim(self) -> None:
        self.sim.status(f"CONNECT {CLIENT_MTU}")
        self._connected = True
        FakeBleakClient.connections += 1
        self._services = load_database(self.sim)
        await self._handle_events()

    @property
    def address(self) -> str:
        return self._address

    @property
    def services(self) -> FakeServices:
        return self._services

    @property
    def is_connected(self) -> bool:
        return self._connected

    @property
    def mtu_size(self) -> int:
        return CLIENT_MTU

    def clear_cache(self) -> None:
        pass

    async def _handle_events(self) -> None:
        for event in self.sim.take_events():
            log.debug("sim event: %s", event)
            kind, *args = event.split()
            if kind == "IND" and int(args[0]) in self._notify:
                # HAP indications are empty: the controller then reads the value.
                callback = self._notify[int(args[0])]
                asyncio.get_running_loop().call_soon(callback, int(args[0]), bytearray())
            elif kind == "DISCONNECTED":
                self._mark_disconnected()

    def _mark_disconnected(self) -> None:
        if self._connected:
            self._connected = False
            if self._disconnected_callback:
                self._disconnected_callback(self)

    def _handle(self, char) -> int:
        return char if isinstance(char, int) else char.handle

    async def read_gatt_char(self, char, **kwargs) -> bytearray:
        if not self._connected:
            raise ble_pairing.BleakError("not connected")
        out, ok = self.sim.command(f"READ {self._handle(char)}")
        await self._handle_events()
        parts = ok.split()
        if parts[1] != "0":
            raise ble_pairing.BleakError(f"read failed: {ok}")
        return bytearray.fromhex(parts[2] if len(parts) > 2 else "")

    async def read_gatt_descriptor(self, handle: int, **kwargs) -> bytearray:
        return await self.read_gatt_char(handle)

    async def write_gatt_char(self, char, data, response: bool = True) -> None:
        if not self._connected:
            raise ble_pairing.BleakError("not connected")
        out, ok = self.sim.command(f"WRITE {self._handle(char)} {bytes(data).hex()}")
        await self._handle_events()
        if ok.split()[1] != "0":
            raise ble_pairing.BleakError(f"write failed: {ok}")

    async def start_notify(self, char, callback, **kwargs) -> None:
        c = char if not isinstance(char, int) else self._services.characteristics[char]
        cccd = c.get_descriptor("00002902-0000-1000-8000-00805f9b34fb")
        self.sim.status(f"SUB {cccd.handle} 1")
        self._notify[c.handle] = callback
        await self._handle_events()

    async def disconnect(self) -> bool:
        if self._connected:
            self.sim.status("DISCONNECT")
            self._mark_disconnected()
        return True


# ---------------------------------------------------------------------------------------------------------------------
# Test.


class Failure(Exception):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)
    print(f"  ok  {message}")


async def wait_for(predicate, timeout: float = 5.0) -> bool:
    for _ in range(int(timeout / 0.05)):
        if predicate():
            return True
        await asyncio.sleep(0.05)
    return predicate()


async def run(binary: str) -> None:
    sim = Simulator(binary)
    try:
        await scenario(sim)
    finally:
        sim.close()


def announce(sim: Simulator, controller: BleController) -> Advertisement:
    adv = read_advertisement(sim)
    device = BLEDevice(adv.address, adv.name, None)
    data = AdvertisementData(
        local_name=adv.name,
        manufacturer_data=adv.manufacturer_data,
        service_data={},
        service_uuids=[],
        tx_power=None,
        rssi=-60,
        platform_data=(),
    )
    controller._device_detected(device, data)
    return adv


async def scenario(sim: Simulator) -> None:
    async def fake_establish_connection(device, name, disconnected_callback, **kwargs):
        client = FakeBleakClient(sim, device.address, disconnected_callback)
        await client.connect_sim()
        return client

    ble_discovery.establish_connection = fake_establish_connection
    ble_pairing.establish_connection = fake_establish_connection

    controller = BleController(char_cache=CharacteristicCacheMemory())

    print("Boot")
    out, ok = sim.command("STAT")
    check(ok == "OK 0", "stock Ruuvi history log removed from flash on first boot")

    print("Discovery")
    adv = announce(sim, controller)
    check(adv.active, "accessory is advertising")
    check(adv.name == "RuuviTag A8CA", f"advertised name is {adv.name!r}")
    check(len(controller.discoveries) == 1, "aiohomekit recognizes the HomeKit advertisement")
    discovery = next(iter(controller.discoveries.values()))
    check(not discovery.description.paired if hasattr(discovery.description, "paired") else True, "accessory is unpaired")
    check(discovery.description.category == 10, f"category is Sensor (got {discovery.description.category})")

    print("Pairing (SRP pair setup with the setup code)")
    sim.status("SENSOR 21.37 45.6")
    sim.status("BATTERY 2.64 21.37")
    finish = await discovery.async_start_pairing("ruuvi")
    pairing = await finish(SETUP_CODE)
    check(pairing is not None, "pair setup completed")
    # Home Assistant registers pairings by accessory ID, which is how broadcasts are routed.
    controller.pairings[pairing.id.lower()] = pairing

    print("Accessory database")
    accessories = await pairing.list_accessories_and_characteristics()
    services = {s["type"].upper(): s for s in accessories[0]["services"]}

    def iid_of(service_type: str, char_type: str) -> int:
        for c in services[service_type.upper()]["characteristics"]:
            if c["type"].upper() == char_type.upper():
                return c["iid"]
        raise KeyError(char_type)

    temp_iid = iid_of(ServicesTypes.TEMPERATURE_SENSOR, CharacteristicsTypes.TEMPERATURE_CURRENT)
    hum_iid = iid_of(ServicesTypes.HUMIDITY_SENSOR, CharacteristicsTypes.RELATIVE_HUMIDITY_CURRENT)
    batt_iid = iid_of(ServicesTypes.BATTERY_SERVICE, CharacteristicsTypes.BATTERY_LEVEL)
    low_iid = iid_of(ServicesTypes.BATTERY_SERVICE, CharacteristicsTypes.STATUS_LO_BATT)
    name_iid = iid_of(ServicesTypes.ACCESSORY_INFORMATION, CharacteristicsTypes.NAME)
    fw_iid = iid_of(ServicesTypes.ACCESSORY_INFORMATION, CharacteristicsTypes.FIRMWARE_REVISION)
    check(True, f"found temperature, humidity, battery and accessory information services ({len(services)} services)")

    print("Reading values")
    values = await pairing.get_characteristics([(1, temp_iid), (1, hum_iid), (1, batt_iid), (1, low_iid), (1, name_iid), (1, fw_iid)])
    v = {iid: values[(1, iid)]["value"] for (_, iid) in values}
    check(abs(v[temp_iid] - 21.4) < 1e-6, f"temperature {v[temp_iid]} °C")
    check(v[hum_iid] == 46, f"humidity {v[hum_iid]} %")
    check(v[batt_iid] == 28, f"battery level {v[batt_iid]} % at 2.64 V")
    check(v[low_iid] == 0, "battery not low at 2.64 V")
    check(v[name_iid] == "RuuviTag A8CA", f"name {v[name_iid]!r}")
    check(v[fw_iid] == "1.1.0", f"firmware revision {v[fw_iid]!r}")

    print("Event notifications (connected)")
    received: list[dict] = []
    pairing.dispatcher_connect(lambda data: received.append(data))
    await pairing.subscribe([(1, temp_iid), (1, hum_iid)])
    sim.status("SENSOR 21.45 45.9")  # Below the event thresholds.
    await asyncio.sleep(0.3)
    check(not received, "small changes do not raise events")
    sim.status("SENSOR 23.0 45.9")
    check(await wait_for(lambda: any((1, temp_iid) in r for r in received)), "temperature change raised an event")
    got = [r[(1, temp_iid)]["value"] for r in received if (1, temp_iid) in r]
    check(abs(got[-1] - 23.0) < 1e-6, f"event value {got[-1]} °C")

    print("Identify")
    sim.take_events()
    await pairing.identify()
    sim.status("TICK 2000")
    leds = [e for e in sim.take_events() if e.startswith("LED")]
    check(len(leds) >= 4, f"LED blinked for identify ({len(leds)} toggles)")

    print("Reconnect (pair verify)")
    connections = FakeBleakClient.connections
    await pairing.close()
    sim.status("TICK 5000")
    sim.status("SENSOR 24.5 50.0")  # Changes while disconnected.
    sim.status("TICK 4000")
    announce(sim, controller)
    values = await pairing.get_characteristics([(1, temp_iid), (1, hum_iid)])
    check(FakeBleakClient.connections > connections, "controller reconnected")
    check(abs(values[(1, temp_iid)]["value"] - 24.5) < 1e-6, "value updated while disconnected is read after pair verify")

    print("Broadcast notifications (disconnected, no connection needed)")
    received.clear()
    await pairing.close()
    sim.status("TICK 5000")
    regular = read_advertisement(sim)
    check(abs(regular.interval_ms - 1022.5) < 1, f"regular advertising interval {regular.interval_ms} ms")
    sim.status("SENSOR 26.0 50.0")
    adv = announce(sim, controller)
    check(adv.manufacturer_data[0x004C][0] == 0x11, "value change is advertised as an encrypted notification")
    if os.environ.get("HK_DEBUG"):
        from aiohomekit.controller.ble.manufacturer_data import HomeKitEncryptedNotification
        n = HomeKitEncryptedNotification.from_manufacturer_data(adv.name, adv.address, adv.manufacturer_data)
        print("DEBUG notification id", n.id, "pairing id", pairing.id, "mfr", adv.manufacturer_data[0x004C].hex())
    check(await wait_for(lambda: any((1, temp_iid) in r for r in received)), "controller decrypted the broadcast")
    got = [r[(1, temp_iid)]["value"] for r in received if (1, temp_iid) in r]
    check(abs(got[-1] - 26.0) < 1e-6, f"broadcast value {got[-1]} °C")
    sim.status("TICK 5000")
    check(read_advertisement(sim).manufacturer_data[0x004C][0] == 0x06, "back to regular HomeKit advertising afterwards")

    print("Event rate limit (changes below the urgent delta wait for the minimum interval)")
    received.clear()
    sim.status("SENSOR 26.5 50.0")
    check(read_advertisement(sim).manufacturer_data[0x004C][0] == 0x06, "0.5 °C change shortly after an event is held back")
    # Reads always get the latest sample. The connection also starts a new disconnected period:
    # the GSN only advances once per period, and aiohomekit drops broadcasts with a reused GSN.
    announce(sim, controller)
    values = await pairing.get_characteristics([(1, temp_iid)])
    check(abs(values[(1, temp_iid)]["value"] - 26.5) < 1e-6, "held back value is returned by reads")
    await pairing.close()
    sim.status("TICK 300000")
    sim.status("SENSOR 26.5 50.0")
    adv = announce(sim, controller)
    check(adv.manufacturer_data[0x004C][0] == 0x11, "held back change is sent after the minimum interval")
    check(await wait_for(lambda: any((1, temp_iid) in r for r in received)), "controller decrypted the delayed broadcast")
    got = [r[(1, temp_iid)]["value"] for r in received if (1, temp_iid) in r]
    check(abs(got[-1] - 26.5) < 1e-6, f"delayed broadcast value {got[-1]} °C")
    # The humidity change (45.6 -> 50 %) came due in the same sample. A new broadcast replaces the
    # running one, so it must wait until the temperature broadcast has run for 3 s.
    check(not any((1, hum_iid) in r for r in received), "second event does not replace the running broadcast")
    sim.status("TICK 3200")
    announce(sim, controller)
    check(await wait_for(lambda: any((1, hum_iid) in r for r in received)), "queued humidity event is broadcast next")
    got = [r[(1, hum_iid)]["value"] for r in received if (1, hum_iid) in r]
    check(abs(got[-1] - 50.0) < 1e-6, f"queued broadcast value {got[-1]} %")
    sim.status("TICK 5000")

    print("Long writes (Prepare / Execute Write)")
    await pairing.close()
    sim.status("TICK 2000")
    adv = read_advertisement(sim)
    sim.status(f"CONNECT {CLIENT_MTU}")
    db = load_database(sim)
    pf = next(c for s in db.services.values() for c in s.characteristics if c.uuid.upper() == f"0000004F-0000-1000-8000-0026BB765291")
    iid = int.from_bytes(bytes.fromhex(sim.status(f"READ {pf.get_descriptor('dc46f0fe-81d2-4616-b5d9-6abdd796939a').handle}")[0]), "little")
    # HAP-Characteristic-Signature-Read (opcode 1), sent as a queued write.
    request = bytes([0x00, 0x01, 0x42]) + iid.to_bytes(2, "little")
    sim.status(f"WRITE {pf.handle} {request.hex()} long")
    response = bytes.fromhex(sim.status(f"READ {pf.handle}")[0])
    check(response[:3] == bytes([0x02, 0x42, 0x00]), f"signature read through a queued write succeeded ({response[:3].hex()})")
    sim.status("DISCONNECT")
    sim.status("TICK 2000")

    print("Remove pairing")
    announce(sim, controller)
    pairings = await pairing.list_pairings()
    check(len(pairings) == 1, "one controller pairing stored")
    await pairing.remove_pairing(pairings[0]["pairingId"])
    await pairing.close()
    sim.status("TICK 3000")
    announce(sim, controller)
    discovery = next(iter(controller.discoveries.values()))
    check(not discovery.paired, "accessory is pairable again after removing the last pairing")

    print("All end-to-end checks passed.")


def main() -> None:
    logging.basicConfig(level=logging.DEBUG if os.environ.get("HK_DEBUG") else logging.WARNING)
    binary = sys.argv[1] if len(sys.argv) > 1 else SIM_BINARY
    try:
        asyncio.run(run(binary))
    except Failure as e:
        print(f"  FAIL {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
