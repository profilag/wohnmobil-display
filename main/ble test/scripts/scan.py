#!/usr/bin/env python3
"""Phase 2 - BLE-Scan.

Sucht ca. 10-15 s nach BLE-Geraeten und zeigt Name, CoreBluetooth-UUID, RSSI,
advertised Services, Hersteller- und Service-Daten.

Es wird nichts verbunden und nichts geschrieben.

    python scripts/scan.py
    python scripts/scan.py --seconds 20 --prefix HS --prefix Humsi
"""

from __future__ import annotations

import argparse
import sys

import _bootstrap  # noqa: F401  (setzt sys.path auf src/)

from humsienk import cli, scanner


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Passiver BLE-Scan (read-only, keine Verbindung).",
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
        help="Namenspraefix, das hervorgehoben wird (mehrfach moeglich, Standard: HS)",
    )
    parser.add_argument(
        "--no-capture",
        action="store_true",
        help="Report nicht nach captures/ schreiben",
    )
    parser.add_argument("--debug", action="store_true", help="Tracebacks anzeigen")
    return parser.parse_args(argv)


async def run_scan(args: argparse.Namespace) -> int:
    prefixes = tuple(args.prefix) if args.prefix else scanner.DEFAULT_NAME_PREFIXES

    print(f"Scanne {args.seconds:.0f} s nach BLE-Geraeten ...")
    print(f"Hervorgehoben werden Namen mit Praefix: {', '.join(prefixes)}")
    print()

    def announce(device: scanner.DiscoveredDevice) -> None:
        mark = ">>>" if device.matches_prefix(prefixes) else "  -"
        rssi = "n/a" if device.rssi is None else f"{device.rssi:>4} dBm"
        print(f"{mark} {rssi}  {device.display_name}")

    devices = await scanner.scan(args.seconds, prefixes=prefixes, on_sighting=announce)

    report = cli.format_scan_report(devices, prefixes=prefixes, seconds=args.seconds)
    print()
    print(report)

    if not args.no_capture:
        path = cli.save_capture("scan", report + "\n", suffix=".txt")
        print()
        print(f"Report gespeichert: {path}")

    return 0 if devices else 2


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return cli.run(lambda: run_scan(args), debug=args.debug)


if __name__ == "__main__":
    sys.exit(main())
