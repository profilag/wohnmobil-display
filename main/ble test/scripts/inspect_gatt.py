#!/usr/bin/env python3
"""Phase 3 - GATT-Inspektor.

Verbindet sich mit einem ausgewaehlten Geraet und listet die komplette
GATT-Struktur: Services, Characteristics, Handles, Properties, Descriptors.

Geschrieben wird NICHTS: kein Handshake, kein Telegramm.
Mit ``--read`` werden lesbare Characteristics zusaetzlich ausgelesen - das ist
eine reine Abfrage und aendert keine Konfiguration.

    python scripts/inspect_gatt.py
    python scripts/inspect_gatt.py --read
    python scripts/inspect_gatt.py --address <CoreBluetooth-UUID>
"""

from __future__ import annotations

import argparse
import sys

import _bootstrap  # noqa: F401  (setzt sys.path auf src/)

from humsienk import cli, gatt, scanner


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="GATT-Struktur eines BLE-Geraets auslesen (read-only).",
    )
    parser.add_argument(
        "--address",
        default=None,
        help="CoreBluetooth-UUID des Geraets (ueberspringt den Scan)",
    )
    parser.add_argument(
        "--seconds",
        type=float,
        default=scanner.DEFAULT_SCAN_SECONDS,
        help=f"Scandauer in Sekunden (Standard: {scanner.DEFAULT_SCAN_SECONDS:.0f})",
    )
    parser.add_argument(
        "--prefix",
        action="append",
        default=None,
        metavar="PREFIX",
        help="Namenspraefix fuer Kandidaten (mehrfach moeglich, Standard: HS)",
    )
    parser.add_argument(
        "--read",
        action="store_true",
        help="Lesbare Characteristics zusaetzlich auslesen (reine Abfrage)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=20.0,
        help="Verbindungs-Timeout in Sekunden (Standard: 20)",
    )
    parser.add_argument(
        "--no-capture",
        action="store_true",
        help="Report nicht nach captures/ schreiben",
    )
    parser.add_argument("--debug", action="store_true", help="Tracebacks anzeigen")
    return parser.parse_args(argv)


async def run_inspect(args: argparse.Namespace) -> int:
    target = await cli.pick_target(
        address=args.address,
        seconds=args.seconds,
        prefixes=tuple(args.prefix) if args.prefix else scanner.DEFAULT_NAME_PREFIXES,
    )
    if target is None:
        return 2
    connect_target, device_name = target

    print()
    print("Verbinde (nur lesend - es wird nichts geschrieben) ...")
    dump = await gatt.dump_gatt(
        connect_target,
        connect_timeout=args.timeout,
        read_values=args.read,
    )

    report = cli.format_gatt_report(dump, device_name=device_name)
    print()
    print(report)

    if not args.read:
        print()
        print(
            "Tipp: mit --read werden lesbare Characteristics zusaetzlich "
            "ausgelesen (z. B. Model/Firmware). Das ist eine reine Abfrage."
        )

    if not args.no_capture:
        path = cli.save_capture("gatt", report + "\n", suffix=".txt")
        print()
        print(f"Report gespeichert: {path}")

    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return cli.run(lambda: run_inspect(args), debug=args.debug)


if __name__ == "__main__":
    sys.exit(main())
