"""UUID-Hilfsfunktionen (Normalisierung + Kurzform + bekannte Namen).

Bleak liefert UUIDs immer als volle 128-Bit-Strings, z. B.

    0000fff1-0000-1000-8000-00805f9b34fb

Fuer die Ausgabe und fuer Vergleiche mit dokumentierten UUIDs ist die
16-Bit-Kurzform (``FFF1``) deutlich lesbarer.
"""

from __future__ import annotations

# Bluetooth Base UUID: xxxxxxxx-0000-1000-8000-00805f9b34fb
_BASE_SUFFIX = "-0000-1000-8000-00805f9b34fb"

# Nur ein kleiner Auszug der SIG-Definitionen: genug, um einen GATT-Dump
# lesbar zu machen, ohne eine komplette Datenbank mitzuschleppen.
_KNOWN_NAMES: dict[str, str] = {
    # Services
    "1800": "Generic Access",
    "1801": "Generic Attribute",
    "180A": "Device Information",
    "180F": "Battery Service",
    "1805": "Current Time",
    "FE59": "Nordic DFU",
    # Characteristics
    "2A00": "Device Name",
    "2A01": "Appearance",
    "2A04": "Peripheral Preferred Connection Parameters",
    "2A05": "Service Changed",
    "2A19": "Battery Level",
    "2A23": "System ID",
    "2A24": "Model Number String",
    "2A25": "Serial Number String",
    "2A26": "Firmware Revision String",
    "2A27": "Hardware Revision String",
    "2A28": "Software Revision String",
    "2A29": "Manufacturer Name String",
    "2A50": "PnP ID",
    # Descriptors
    "2900": "Characteristic Extended Properties",
    "2901": "Characteristic User Description",
    "2902": "Client Characteristic Configuration (CCCD)",
    "2903": "Server Characteristic Configuration",
    "2904": "Characteristic Presentation Format",
}


def normalize(uuid: str) -> str:
    """Bringt eine 16-/32-/128-Bit-UUID auf die volle Kleinbuchstaben-Form."""
    text = uuid.strip().lower().removeprefix("0x")
    if len(text) in (4, 8):
        return f"{int(text, 16):08x}{_BASE_SUFFIX}"
    return text


def short(uuid: str) -> str:
    """Kurzform fuer SIG-Basis-UUIDs, sonst die vollstaendige UUID.

    ``0000fff1-0000-1000-8000-00805f9b34fb`` -> ``FFF1``
    Eine echte Vendor-UUID bleibt unveraendert (nur in Grossbuchstaben).
    """
    text = normalize(uuid)
    if not text.endswith(_BASE_SUFFIX):
        return uuid.strip().upper()
    prefix = text[:8]
    if prefix.startswith("0000"):
        return prefix[4:].upper()
    return prefix.upper()


def same(a: str, b: str) -> bool:
    """Vergleicht zwei UUIDs unabhaengig von Schreibweise und Laenge."""
    return normalize(a) == normalize(b)


def known_name(uuid: str) -> str | None:
    """Bekannter SIG-Name zu einer UUID, falls vorhanden."""
    return _KNOWN_NAMES.get(short(uuid))


def describe(uuid: str) -> str:
    """``FFF1`` bzw. ``2A24 (Model Number String)`` fuer die Ausgabe."""
    label = short(uuid)
    name = known_name(uuid)
    return f"{label} ({name})" if name else label
