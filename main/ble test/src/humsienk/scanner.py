"""Phase 2 - passiver BLE-Scan.

Dieses Modul verbindet sich NICHT und schreibt NICHT. Es sammelt nur
Advertising-Daten.

macOS-Besonderheit
------------------
CoreBluetooth gibt Anwendungen keine klassische Bluetooth-MAC-Adresse heraus.
``BLEDevice.address`` ist auf macOS eine CoreBluetooth-Peripheral-UUID, die pro
Mac (und teils pro Pairing) unterschiedlich ist. Sie ist als Handle fuer
``BleakClient`` gueltig, taugt aber nicht als geraeteglobale Kennung.
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field

from bleak import BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

DEFAULT_SCAN_SECONDS = 12.0
DEFAULT_NAME_PREFIXES: tuple[str, ...] = ("HS",)


@dataclass
class DiscoveredDevice:
    """Ueber mehrere Advertising-Pakete zusammengefuehrter Geraeteeintrag.

    Ein BLE-Geraet verteilt seine Daten oft auf mehrere Pakete (Name im
    Scan-Response, Hersteller-Daten im Advertisement). Deshalb wird pro Adresse
    akkumuliert statt nur das letzte Paket zu behalten.
    """

    address: str
    device: BLEDevice
    name: str | None = None
    rssi: int | None = None
    rssi_best: int | None = None
    tx_power: int | None = None
    service_uuids: set[str] = field(default_factory=set)
    manufacturer_data: dict[int, bytes] = field(default_factory=dict)
    service_data: dict[str, bytes] = field(default_factory=dict)
    packets: int = 0

    def update(self, device: BLEDevice, adv: AdvertisementData) -> bool:
        """Fuegt ein weiteres Advertising-Paket ein.

        Rueckgabe: True, wenn dadurch erstmals ein Geraetename bekannt wurde.
        """
        self.device = device
        self.packets += 1

        name_is_new = False
        name = adv.local_name or device.name
        if name and not self.name:
            name_is_new = True
        if name:
            self.name = name

        if adv.rssi is not None:
            self.rssi = adv.rssi
            if self.rssi_best is None or adv.rssi > self.rssi_best:
                self.rssi_best = adv.rssi

        if adv.tx_power is not None:
            self.tx_power = adv.tx_power

        self.service_uuids.update(adv.service_uuids or ())
        self.manufacturer_data.update(adv.manufacturer_data or {})
        self.service_data.update(adv.service_data or {})
        return name_is_new

    @property
    def display_name(self) -> str:
        return self.name or "(kein Name)"

    def matches_prefix(self, prefixes: Sequence[str]) -> bool:
        """True, wenn der Geraetename mit einem der Praefixe beginnt."""
        if not self.name:
            return False
        upper = self.name.upper()
        return any(upper.startswith(p.upper()) for p in prefixes if p)


async def scan(
    seconds: float = DEFAULT_SCAN_SECONDS,
    *,
    prefixes: Sequence[str] = DEFAULT_NAME_PREFIXES,
    on_sighting: Callable[[DiscoveredDevice], None] | None = None,
) -> list[DiscoveredDevice]:
    """Scannt ``seconds`` lang und liefert die gefundenen Geraete.

    Sortierung: Praefix-Treffer zuerst, dann nach Signalstaerke.

    ``on_sighting`` wird - falls gesetzt - fuer die Live-Ausgabe aufgerufen:
    einmal beim erstmaligen Sichten eines Geraets und noch einmal, falls sein
    Name erst in einem spaeteren Paket (Scan-Response) eintrifft.
    """
    found: dict[str, DiscoveredDevice] = {}

    def callback(device: BLEDevice, adv: AdvertisementData) -> None:
        entry = found.get(device.address)
        is_new = entry is None
        if entry is None:
            entry = DiscoveredDevice(address=device.address, device=device)
            found[device.address] = entry

        name_is_new = entry.update(device, adv)

        if on_sighting is not None and (is_new or name_is_new):
            on_sighting(entry)

    scanner = BleakScanner(detection_callback=callback)
    async with scanner:
        await asyncio.sleep(seconds)

    def sort_key(entry: DiscoveredDevice) -> tuple[int, int, str]:
        rssi = entry.rssi_best if entry.rssi_best is not None else -999
        return (0 if entry.matches_prefix(prefixes) else 1, -rssi, entry.display_name)

    return sorted(found.values(), key=sort_key)


async def find_candidates(
    seconds: float = DEFAULT_SCAN_SECONDS,
    *,
    prefixes: Sequence[str] = DEFAULT_NAME_PREFIXES,
) -> tuple[list[DiscoveredDevice], list[DiscoveredDevice]]:
    """Scannt und trennt in (Kandidaten, alle Geraete).

    Kandidat = Name beginnt mit einem der Praefixe.
    """
    devices = await scan(seconds, prefixes=prefixes)
    candidates = [d for d in devices if d.matches_prefix(prefixes)]
    return candidates, devices


async def resolve_device(address: str, seconds: float = 10.0) -> BLEDevice | None:
    """Sucht ein konkretes Geraet anhand seiner Adresse / CoreBluetooth-UUID.

    Auf macOS ist ein frisch gescanntes ``BLEDevice`` als Verbindungsziel
    zuverlaessiger als ein reiner Adress-String.
    """
    return await BleakScanner.find_device_by_address(address, timeout=seconds)
