"""Gemeinsame CLI-Infrastruktur: Formatierung, Fehlermeldungen, Capture-Dateien.

Bewusst getrennt von ``scanner.py`` / ``gatt.py``: dort steht nur BLE-Logik,
hier nur Darstellung und Benutzerfuehrung.
"""

from __future__ import annotations

import asyncio
import datetime as _dt
import json as _json
import sys
import traceback
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import Sequence

from . import scanner, uuids
from .gatt import GattDump, documented_uuid_report
from .rawlog import ListenResult, RawFrame, Subscription
from .scanner import DiscoveredDevice

PROJECT_ROOT = Path(__file__).resolve().parents[2]
CAPTURES_DIR = PROJECT_ROOT / "captures"


# --------------------------------------------------------------------------- #
# Bytes / Zeit
# --------------------------------------------------------------------------- #

def hex_bytes(data: bytes, *, sep: str = " ") -> str:
    """``7E 00 8C 03``"""
    return sep.join(f"{b:02X}" for b in data)


def ascii_preview(data: bytes) -> str:
    """Druckbare Zeichen, alles andere als ``.`` - nur als Lesehilfe."""
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data)


def timestamp(fmt: str = "%Y-%m-%d_%H%M%S") -> str:
    return _dt.datetime.now().strftime(fmt)


def iso_now() -> str:
    return _dt.datetime.now().isoformat(timespec="milliseconds")


# --------------------------------------------------------------------------- #
# Capture-Dateien
# --------------------------------------------------------------------------- #

def capture_path(prefix: str, *, suffix: str = ".log", stamp: str | None = None) -> Path:
    """Pfad ``captures/<prefix>_<zeitstempel><suffix>`` - schreibt noch nichts.

    Fuer Logger, die mitlaufend schreiben statt am Ende einmal.
    ``stamp`` erlaubt mehreren Dateien denselben Zeitstempel.
    """
    CAPTURES_DIR.mkdir(parents=True, exist_ok=True)
    return CAPTURES_DIR / f"{prefix}_{stamp or timestamp()}{suffix}"


def save_capture(prefix: str, text: str, *, suffix: str = ".log") -> Path:
    """Schreibt Text nach ``captures/<prefix>_<zeitstempel><suffix>``."""
    path = capture_path(prefix, suffix=suffix)
    path.write_text(text, encoding="utf-8")
    return path


# --------------------------------------------------------------------------- #
# Scan-Ausgabe
# --------------------------------------------------------------------------- #

def format_device(device: DiscoveredDevice, *, highlight: bool = False) -> str:
    marker = ">>>" if highlight else "   "
    lines = [f"{marker} {device.display_name}"]
    pad = "      "

    lines.append(f"{pad}address (CoreBluetooth UUID): {device.address}")

    rssi = "n/a" if device.rssi is None else f"{device.rssi} dBm"
    best = "" if device.rssi_best in (None, device.rssi) else f" (best {device.rssi_best} dBm)"
    lines.append(f"{pad}rssi:        {rssi}{best}")

    if device.tx_power is not None:
        lines.append(f"{pad}tx_power:    {device.tx_power} dBm")

    lines.append(f"{pad}adv packets: {device.packets}")

    if device.service_uuids:
        lines.append(f"{pad}services:")
        for uuid in sorted(device.service_uuids):
            lines.append(f"{pad}  {uuids.describe(uuid)}   [{uuid}]")
    else:
        lines.append(f"{pad}services:    (keine im Advertisement)")

    if device.manufacturer_data:
        lines.append(f"{pad}manufacturer data:")
        for company_id, payload in sorted(device.manufacturer_data.items()):
            lines.append(
                f"{pad}  0x{company_id:04X} ({len(payload)} B): {hex_bytes(payload)}"
            )
            lines.append(f"{pad}         ascii: {ascii_preview(payload)}")

    if device.service_data:
        lines.append(f"{pad}service data:")
        for uuid, payload in sorted(device.service_data.items()):
            lines.append(
                f"{pad}  {uuids.short(uuid)} ({len(payload)} B): {hex_bytes(payload)}"
            )

    return "\n".join(lines)


def format_scan_report(
    devices: Sequence[DiscoveredDevice],
    *,
    prefixes: Sequence[str],
    seconds: float,
) -> str:
    prefix_text = ", ".join(prefixes) if prefixes else "(keine)"
    header = [
        "BLE SCAN REPORT",
        "=" * 72,
        f"zeit:      {iso_now()}",
        f"dauer:     {seconds:.0f} s",
        f"praefixe:  {prefix_text}",
        f"geraete:   {len(devices)}",
        "",
        "Hinweis: macOS/CoreBluetooth liefert keine Bluetooth-MAC-Adresse,",
        "sondern eine Mac-lokale Peripheral-UUID.",
        "",
    ]

    if not devices:
        header.append("Keine BLE-Geraete gefunden.")
        return "\n".join(header)

    blocks: list[str] = []
    for device in devices:
        blocks.append(format_device(device, highlight=device.matches_prefix(prefixes)))
    body = "\n\n".join(blocks)

    candidates = [d for d in devices if d.matches_prefix(prefixes)]
    footer = ["", "-" * 72]
    if candidates:
        footer.append(f"{len(candidates)} Kandidat(en) mit passendem Namenspraefix:")
        for device in candidates:
            footer.append(f"  {device.display_name}  ->  {device.address}")
    else:
        footer.append(
            f"Kein Geraet mit Praefix {prefix_text} gefunden. "
            "Ggf. --seconds erhoehen oder --prefix anpassen."
        )

    return "\n".join([*header, body, *footer])


# --------------------------------------------------------------------------- #
# GATT-Ausgabe
# --------------------------------------------------------------------------- #

def _flags(char) -> str:
    flags = []
    if char.readable:
        flags.append("read")
    if char.writable:
        flags.append("write")
    if char.notifiable:
        flags.append("notify")
    if char.indicatable:
        flags.append("indicate")
    return ", ".join(flags) if flags else "-"


def format_gatt_report(dump: GattDump, *, device_name: str | None = None) -> str:
    name = device_name or dump.name or "(kein Name)"
    lines = [
        "GATT REPORT",
        "=" * 72,
        f"zeit:     {iso_now()}",
        f"geraet:   {name}",
        f"address:  {dump.address}",
        f"services: {len(dump.services)}",
        "",
    ]

    for service in dump.services:
        handle = "" if service.handle is None else f"  [handle {service.handle}]"
        lines.append(f"SERVICE {uuids.describe(service.uuid)}{handle}")
        lines.append(f"  uuid: {service.uuid}")
        if not service.characteristics:
            lines.append("  (keine Characteristics)")
        for char in service.characteristics:
            lines.append("")
            chandle = "" if char.handle is None else f"  [handle {char.handle}]"
            lines.append(f"  CHARACTERISTIC {uuids.describe(char.uuid)}{chandle}")
            lines.append(f"    {'uuid:':<14}{char.uuid}")
            lines.append(f"    {'properties:':<14}{', '.join(char.properties) or '-'}")
            lines.append(f"    {'capabilities:':<14}{_flags(char)}")
            if char.max_write_without_response is not None:
                lines.append(
                    f"    {'max write:':<14}{char.max_write_without_response} B "
                    "(ohne Response)"
                )
            if char.descriptors:
                lines.append(f"    {'descriptors:':<14}")
                for desc in char.descriptors:
                    dh = "" if desc.handle is None else f"  [handle {desc.handle}]"
                    lines.append(f"      {uuids.describe(desc.uuid)}{dh}")
            else:
                lines.append(f"    {'descriptors:':<14}-")
            if char.value is not None:
                lines.append(
                    f"    {'value:':<14}{len(char.value)} B  {hex_bytes(char.value)}"
                )
                lines.append(f"    {'ascii:':<14}{ascii_preview(char.value)}")
            elif char.read_error:
                lines.append(f"    {'read failed:':<14}{char.read_error}")
        lines.append("")
        lines.append("-" * 72)

    lines.append("")
    lines.append("Abgleich mit dokumentierten WATT/HiLink-UUIDs:")
    report = documented_uuid_report(dump)
    for uuid, present in report.items():
        lines.append(f"  {uuid}: {'gefunden' if present else 'NICHT gefunden'}")
    if all(report.values()):
        lines.append("")
        lines.append(
            "Alle dokumentierten UUIDs vorhanden. Die Protokoll-Klassifizierung "
            "erfolgt in Phase 4 - hier wird noch nichts geschrieben."
        )
    else:
        lines.append("")
        lines.append(
            "Struktur weicht von der Dokumentation ab. Keine Annahmen treffen, "
            "GATT-Dump auswerten."
        )

    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Geraeteauswahl (gemeinsam fuer alle Skripte, die sich verbinden)
# --------------------------------------------------------------------------- #

async def choose_device(devices: Sequence[DiscoveredDevice]) -> DiscoveredDevice | None:
    """Interaktive Auswahl, wenn mehrere Geraete in Frage kommen."""
    print()
    print("Mehrere Geraete gefunden:")
    for index, device in enumerate(devices, start=1):
        rssi = "n/a" if device.rssi is None else f"{device.rssi} dBm"
        print(f"  [{index}] {device.display_name}  ({rssi})  {device.address}")
    print("  [0] abbrechen")

    while True:
        answer = (await asyncio.to_thread(input, "Auswahl: ")).strip()
        if answer in ("", "0", "q"):
            return None
        if answer.isdigit() and 1 <= int(answer) <= len(devices):
            return devices[int(answer) - 1]
        print("Ungueltige Eingabe.")


async def pick_target(
    *,
    address: str | None = None,
    seconds: float = scanner.DEFAULT_SCAN_SECONDS,
    prefixes: Sequence[str] = scanner.DEFAULT_NAME_PREFIXES,
) -> tuple[object, str | None] | None:
    """Liefert (Verbindungsziel, Anzeigename) oder None bei Abbruch.

    Mit ``address`` wird der Scan uebersprungen; ein frisch gescanntes
    ``BLEDevice`` ist auf macOS als Verbindungsziel aber zuverlaessiger als ein
    reiner Adress-String, deshalb wird kurz danach gesucht.
    """
    if address:
        print(f"Suche Geraet {address} ...")
        device = await scanner.resolve_device(address, seconds=min(seconds, 10.0))
        if device is None:
            print(
                "Geraet nicht im Scan gefunden - versuche direkte Verbindung "
                "mit der angegebenen Adresse.",
                file=sys.stderr,
            )
            return address, None
        return device, device.name

    print(f"Scanne {seconds:.0f} s (Praefix: {', '.join(prefixes)}) ...")
    candidates, all_devices = await scanner.find_candidates(seconds, prefixes=prefixes)

    selectable = list(candidates)
    if not candidates:
        named = [d for d in all_devices if d.name]
        if not named:
            print("Keine benannten BLE-Geraete gefunden.", file=sys.stderr)
            return None
        print(
            f"Kein Geraet mit Praefix {', '.join(prefixes)} gefunden - "
            "zeige alle benannten Geraete."
        )
        selectable = named

    if len(selectable) == 1:
        device = selectable[0]
        print(f"Verwende: {device.display_name}  ({device.address})")
        return device, device.name

    chosen = await choose_device(selectable)
    if chosen is None:
        return None
    return chosen, chosen.name


# --------------------------------------------------------------------------- #
# Roh-Log-Ausgabe (Phase 7)
# --------------------------------------------------------------------------- #

def format_frame(frame: RawFrame) -> str:
    """Zwei Zeilen pro Paket: Kopf mit Hex, darunter die ASCII-Lesehilfe.

    Die ASCII-Zeile wird unter die Hex-Spalte eingerueckt, damit Byte und
    Zeichen im Log uebereinander stehen.
    """
    prefix = (
        f"#{frame.seq:04d}  +{frame.offset:8.3f} s  {frame.direction}  "
        f"{frame.short_uuid}  ({len(frame.data):3d} B)  "
    )
    ascii_line = " ".join(f" {c}" for c in ascii_preview(frame.data))
    return f"{prefix}{hex_bytes(frame.data)}\n{' ' * len(prefix)}{ascii_line}"


def frame_as_json(frame: RawFrame) -> str:
    """Eine JSONL-Zeile - maschinenlesbares Gegenstueck fuer Phase 8."""
    return _json.dumps(
        {
            "seq": frame.seq,
            "offset": round(frame.offset, 4),
            "iso": frame.iso,
            "dir": frame.direction,
            "uuid": frame.char_uuid,
            "short": frame.short_uuid,
            "handle": frame.handle,
            "len": len(frame.data),
            "hex": hex_bytes(frame.data, sep=""),
        },
        ensure_ascii=False,
    )


def format_listen_header(
    *, address: str, name: str | None, seconds: float
) -> str:
    """Kopf des Roh-Logs - steht vor dem ersten Paket in der Datei.

    Enthaelt nur, was schon vor dem Verbinden bekannt ist; die Abonnements
    kommen einzeln ueber ``format_subscription`` dazu.
    """
    limit = "unbegrenzt (bis Ctrl-C)" if seconds <= 0 else f"{seconds:.0f} s"
    return "\n".join(
        [
            "BLE RAW NOTIFY LOG (Phase 7 - passiv, es wird kein Telegramm gesendet)",
            "=" * 72,
            f"zeit:     {iso_now()}",
            f"geraet:   {name or '(kein Name)'}",
            f"address:  {address}",
            f"laufzeit: {limit}",
            "",
            "Hinweis: 'start_notify' schreibt das CCCD (0x2902) der Characteristic.",
            "Das ist der Standardweg fuer ein Abonnement und kein BMS-Parameter.",
            "",
            "Abonnements:",
        ]
    )


def format_subscription(sub: Subscription) -> str:
    """Eine Zeile pro ``start_notify``-Versuch."""
    state = "aktiv" if sub.active else f"FEHLER ({sub.error})"
    handle = "" if sub.handle is None else f"  [handle {sub.handle}]"
    props = ", ".join(sub.properties) or "-"
    return f"  {sub.short_uuid}{handle}  {props}  ->  {state}"


def _interval_stats(frames: Sequence[RawFrame]) -> str | None:
    """min/mittel/max Abstand zwischen aufeinanderfolgenden Paketen."""
    if len(frames) < 2:
        return None
    deltas = [b.offset - a.offset for a, b in zip(frames, frames[1:])]
    mean = sum(deltas) / len(deltas)
    return f"min {min(deltas):.3f} s | mittel {mean:.3f} s | max {max(deltas):.3f} s"


def _framing_hint(frames: Sequence[RawFrame]) -> list[str]:
    """Rein deskriptiv: passen die Paketgrenzen zum vermuteten 7E..0D-Rahmen?

    Keine Protokollentscheidung - die faellt in Phase 4 anhand der Rohdaten.
    """
    if not frames:
        return []
    starts_7e = sum(1 for f in frames if f.data[:1] == b"\x7e")
    ends_0d = sum(1 for f in frames if f.data[-1:] == b"\x0d")
    total = len(frames)
    return [
        "",
        "Rahmen-Indizien (nur Beobachtung, keine Klassifizierung):",
        f"  Pakete mit Startbyte 0x7E: {starts_7e}/{total}",
        f"  Pakete mit Endbyte   0x0D: {ends_0d}/{total}",
    ]


def format_listen_summary(result: ListenResult) -> str:
    """Abschlussbericht - wird an den Roh-Log angehaengt und ausgegeben."""
    lines = [
        "",
        "-" * 72,
        "ZUSAMMENFASSUNG",
        f"  laufzeit:   {result.elapsed:.1f} s",
        f"  abos aktiv: {len(result.active_subscriptions)}"
        f" von {len(result.subscriptions)}",
        f"  pakete:     {len(result.frames)}",
    ]

    if result.disconnected:
        lines.append("  ereignis:   Verbindung wurde vom Geraet getrennt")
    if result.stopped_by_user:
        lines.append("  ereignis:   per Ctrl-C beendet")

    grouped = result.frames_by_char()
    if grouped:
        lines.append("")
        lines.append("Pro Characteristic:")
        for short_uuid, frames in sorted(grouped.items()):
            sizes = sorted({len(f.data) for f in frames})
            size_text = (
                f"{sizes[0]} B" if len(sizes) == 1 else f"{sizes[0]}-{sizes[-1]} B"
            )
            lines.append(f"  {short_uuid}: {len(frames)} Pakete, {size_text}")
            stats = _interval_stats(frames)
            if stats:
                lines.append(f"    abstand: {stats}")
            unique = len({bytes(f.data) for f in frames})
            lines.append(f"    davon inhaltlich verschieden: {unique}")

    lines.extend(_framing_hint(result.frames))

    lines.append("")
    if not result.subscriptions:
        lines.append(
            "Das Geraet hat keine abonnierbare Characteristic. Ohne Notify gibt es\n"
            "keinen passiven Weg zu Daten."
        )
    elif not result.frames:
        lines.append(
            "Kein einziges Paket empfangen: dieses BMS sendet nicht von selbst,\n"
            "sondern antwortet nur auf Anfragen. Ein passives Mitlesen genuegt hier\n"
            "nicht.\n"
            "\n"
            "Offene Optionen (beide brauchen eine ausdrueckliche Entscheidung):\n"
            "  a) Sniffen statt raten - HumsiENK-App am Android-Handy mit\n"
            "     aktiviertem Bluetooth-HCI-Snoop-Log betreiben und das Log\n"
            "     auswerten. Bleibt vom Mac aus vollstaendig passiv.\n"
            "  b) Einen dokumentierten Read-Frame senden - erst nach Freigabe,\n"
            "     Phase 4/5, und nur mit Funktionscode Lesen."
        )
    else:
        lines.append(
            "Rohdaten liegen vor. Naechster Schritt: Phase 4 - Struktur der Pakete\n"
            "gegen die Frame-Hypothese pruefen (Laenge, Startbyte, CRC), weiterhin\n"
            "ohne Schreibzugriff."
        )

    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Fehlerbehandlung (Phase 11, Grundstock)
# --------------------------------------------------------------------------- #

_HINTS: tuple[tuple[tuple[str, ...], str], ...] = (
    (
        ("not authorized", "unauthorized", "permission", "denied", "tcc"),
        "Bluetooth-Berechtigung fehlt.\n"
        "  Systemeinstellungen -> Datenschutz & Sicherheit -> Bluetooth\n"
        "  Terminal bzw. Visual Studio Code aktivieren und die App neu starten.",
    ),
    (
        ("turned off", "powered off", "poweredoff", "bluetooth device is turned off"),
        "Bluetooth ist ausgeschaltet. Bitte in den Systemeinstellungen aktivieren.",
    ),
    (
        ("unsupported", "not supported"),
        "Der Bluetooth-Adapter meldet 'unsupported'. Bluetooth-Hardware pruefen.",
    ),
    (
        ("was not found", "not found", "no device"),
        "Geraet nicht gefunden. Ist der Akku in Reichweite und wach?\n"
        "  Pruefen: ist die HumsiENK-App auf dem Smartphone noch verbunden?\n"
        "  Viele BMS erlauben nur EINE aktive BLE-Verbindung.",
    ),
    (
        ("disconnected", "connection lost", "was not connected"),
        "Verbindung verloren. Naeher an den Akku gehen und erneut versuchen.\n"
        "  Auch hier: Smartphone-App trennen.",
    ),
    (
        ("timed out", "timeout"),
        "Timeout. Geraet antwortet nicht.\n"
        "  Moegliche Ursache: BMS ist mit einem anderen Geraet verbunden.",
    ),
    (
        ("failed to connect", "connection failed"),
        "Verbindung fehlgeschlagen.\n"
        "  Smartphone-App/Bluetooth dort trennen und erneut versuchen.",
    ),
)


def friendly_error(exc: BaseException) -> str:
    """Kurze, verstaendliche Meldung statt Traceback."""
    message = str(exc) or type(exc).__name__
    lowered = message.lower()
    for needles, hint in _HINTS:
        if any(needle in lowered for needle in needles):
            return f"{hint}\n\n  Original: {type(exc).__name__}: {message}"
    if isinstance(exc, (asyncio.TimeoutError, TimeoutError)):
        return "Timeout - das Geraet hat nicht rechtzeitig geantwortet."
    if isinstance(exc, ImportError):
        return (
            "Abhaengigkeit fehlt. Bitte ausfuehren:\n"
            "  python3 -m venv .venv && source .venv/bin/activate\n"
            "  pip install -r requirements.txt\n\n"
            f"  Original: {message}"
        )
    return f"Fehler: {type(exc).__name__}: {message}"


def run(main: Callable[[], Awaitable[int]], *, debug: bool = False) -> int:
    """Startet eine async-main und uebersetzt Fehler in Klartext.

    Tracebacks nur mit ``--debug``.
    """
    try:
        return asyncio.run(main())
    except KeyboardInterrupt:
        print("\nAbgebrochen.", file=sys.stderr)
        return 130
    except BaseException as exc:  # noqa: BLE001 - Top-Level-Handler
        if debug:
            traceback.print_exc()
        print("", file=sys.stderr)
        print(friendly_error(exc), file=sys.stderr)
        if not debug:
            print("\n  Details mit --debug", file=sys.stderr)
        return 1
