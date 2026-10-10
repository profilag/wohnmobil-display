"""Phase 3 - GATT-Discovery.

Liest die komplette GATT-Struktur eines Geraets aus und liefert sie als
plattform-unabhaengige Datenklassen zurueck. Es wird ausschliesslich
verbunden, entdeckt und (optional) gelesen.

Es wird NICHTS geschrieben - kein Handshake, kein Telegramm, keine CCCD.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Sequence

from bleak import BleakClient
from bleak.backends.device import BLEDevice

from . import uuids

#: UUIDs, die bei anderen HumsiENK-Geraeten als WATT/HiLink-Struktur
#: dokumentiert wurden. Nur ein Vergleichsraster - keine Klassifizierung.
DOCUMENTED_HILINK_UUIDS: tuple[str, ...] = ("FFF0", "FFF1", "FFF2", "FFFA")


@dataclass(frozen=True)
class DescriptorInfo:
    uuid: str
    handle: int | None


@dataclass(frozen=True)
class CharacteristicInfo:
    uuid: str
    handle: int | None
    properties: tuple[str, ...]
    descriptors: tuple[DescriptorInfo, ...] = ()
    max_write_without_response: int | None = None
    value: bytes | None = None
    read_error: str | None = None

    @property
    def readable(self) -> bool:
        return "read" in self.properties

    @property
    def writable(self) -> bool:
        return "write" in self.properties or "write-without-response" in self.properties

    @property
    def notifiable(self) -> bool:
        return "notify" in self.properties

    @property
    def indicatable(self) -> bool:
        return "indicate" in self.properties

    @property
    def short_uuid(self) -> str:
        return uuids.short(self.uuid)


@dataclass(frozen=True)
class ServiceInfo:
    uuid: str
    handle: int | None
    characteristics: tuple[CharacteristicInfo, ...] = ()

    @property
    def short_uuid(self) -> str:
        return uuids.short(self.uuid)


@dataclass(frozen=True)
class GattDump:
    address: str
    name: str | None
    services: tuple[ServiceInfo, ...]

    def characteristics(self) -> list[CharacteristicInfo]:
        return [c for s in self.services for c in s.characteristics]

    def short_uuids(self) -> set[str]:
        """Alle Service- und Characteristic-UUIDs in Kurzform."""
        result = {s.short_uuid for s in self.services}
        result.update(c.short_uuid for c in self.characteristics())
        return result

    def find(self, uuid: str) -> CharacteristicInfo | None:
        for char in self.characteristics():
            if uuids.same(char.uuid, uuid):
                return char
        return None


def _properties(obj: object) -> tuple[str, ...]:
    return tuple(getattr(obj, "properties", ()) or ())


async def dump_gatt(
    target: str | BLEDevice,
    *,
    connect_timeout: float = 20.0,
    read_values: bool = False,
    read_uuids: Sequence[str] | None = None,
) -> GattDump:
    """Verbindet, liest die GATT-Struktur und trennt wieder.

    ``read_values``  liest zusaetzlich alle lesbaren Characteristics aus.
    ``read_uuids``   beschraenkt das Lesen auf diese UUIDs (falls gesetzt).

    Lesen ist eine reine Abfrage und veraendert keine Konfiguration.
    """
    wanted = {uuids.normalize(u) for u in read_uuids} if read_uuids else None

    async with BleakClient(target, timeout=connect_timeout) as client:
        services: list[ServiceInfo] = []

        for service in client.services:
            chars: list[CharacteristicInfo] = []

            for char in service.characteristics:
                descriptors = tuple(
                    DescriptorInfo(uuid=str(d.uuid), handle=getattr(d, "handle", None))
                    for d in getattr(char, "descriptors", ()) or ()
                )
                props = _properties(char)

                value: bytes | None = None
                read_error: str | None = None
                should_read = (
                    read_values
                    and "read" in props
                    and (wanted is None or uuids.normalize(str(char.uuid)) in wanted)
                )
                if should_read:
                    try:
                        value = bytes(await client.read_gatt_char(char))
                    except Exception as exc:  # Read darf einzeln fehlschlagen
                        read_error = f"{type(exc).__name__}: {exc}"

                chars.append(
                    CharacteristicInfo(
                        uuid=str(char.uuid),
                        handle=getattr(char, "handle", None),
                        properties=props,
                        descriptors=descriptors,
                        max_write_without_response=getattr(
                            char, "max_write_without_response_size", None
                        ),
                        value=value,
                        read_error=read_error,
                    )
                )

            services.append(
                ServiceInfo(
                    uuid=str(service.uuid),
                    handle=getattr(service, "handle", None),
                    characteristics=tuple(chars),
                )
            )

        name = getattr(target, "name", None) if not isinstance(target, str) else None
        address = target if isinstance(target, str) else target.address
        return GattDump(address=address, name=name, services=tuple(services))


def documented_uuid_report(dump: GattDump) -> dict[str, bool]:
    """Welche der dokumentierten HiLink-UUIDs sind vorhanden?

    Rein deskriptiv. Die Protokoll-Entscheidung passiert in Phase 4
    (``protocol.py``), nicht hier.
    """
    present = dump.short_uuids()
    return {u: u in present for u in DOCUMENTED_HILINK_UUIDS}
