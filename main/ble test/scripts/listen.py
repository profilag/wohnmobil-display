#!/usr/bin/env python3
"""Phase 7 - passiver Roh-Logger.

Verbindet sich, abonniert alle Notify-/Indicate-Characteristics und schreibt
mit, was der Akku von selbst sendet. Es wird **kein Telegramm gesendet** -
kein Auth, kein Read, kein Konfigurationszugriff.

Einzige Ausnahme: ``start_notify`` schreibt das CCCD (0x2902) der jeweiligen
Characteristic. Das ist der Standardweg fuer ein Abonnement und beruehrt keinen
BMS-Parameter.

Jedes Paket wird sofort in die Log-Datei geschrieben, nicht erst am Ende -
ein Abbruch per Ctrl-C kostet keine Rohdaten.

    python scripts/listen.py --address <CoreBluetooth-UUID>
    python scripts/listen.py --seconds 180
    python scripts/listen.py --seconds 0            # bis Ctrl-C
    python scripts/listen.py --notify-uuid 0003
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import signal
import sys

import _bootstrap  # noqa: F401  (setzt sys.path auf src/)

from humsienk import cli, rawlog, scanner


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "BLE-Notifications passiv mitschreiben (es wird kein Telegramm gesendet)."
        ),
    )
    parser.add_argument(
        "--address",
        default=None,
        help="CoreBluetooth-UUID des Geraets (ueberspringt den Scan)",
    )
    parser.add_argument(
        "--seconds",
        type=float,
        default=rawlog.DEFAULT_LISTEN_SECONDS,
        help=(
            "Aufzeichnungsdauer in Sekunden, 0 = bis Ctrl-C "
            f"(Standard: {rawlog.DEFAULT_LISTEN_SECONDS:.0f})"
        ),
    )
    parser.add_argument(
        "--scan-seconds",
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
        "--notify-uuid",
        action="append",
        default=None,
        metavar="UUID",
        help="Nur diese Characteristic(s) abonnieren (Kurzform wie 0003 genuegt)",
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
        help="Nichts nach captures/ schreiben (nur Bildschirmausgabe)",
    )
    parser.add_argument("--debug", action="store_true", help="Tracebacks anzeigen")
    return parser.parse_args(argv)


def install_sigint(stop: asyncio.Event) -> None:
    """Ctrl-C beendet die Aufzeichnung geordnet statt mit Traceback.

    Ohne das wuerde der Abschlussbericht bei Abbruch fehlen.
    """
    loop = asyncio.get_running_loop()
    with contextlib.suppress(NotImplementedError):
        loop.add_signal_handler(signal.SIGINT, stop.set)


async def run_listen(args: argparse.Namespace) -> int:
    target = await cli.pick_target(
        address=args.address,
        seconds=args.scan_seconds,
        prefixes=tuple(args.prefix) if args.prefix else scanner.DEFAULT_NAME_PREFIXES,
    )
    if target is None:
        return 2
    connect_target, device_name = target

    stop = asyncio.Event()
    install_sigint(stop)

    address = connect_target if isinstance(connect_target, str) else connect_target.address

    stamp = cli.timestamp()
    log_path = None
    jsonl_path = None
    log_file = None
    jsonl_file = None
    if not args.no_capture:
        log_path = cli.capture_path("raw", suffix=".log", stamp=stamp)
        jsonl_path = cli.capture_path("raw", suffix=".jsonl", stamp=stamp)
        log_file = log_path.open("w", encoding="utf-8")
        jsonl_file = jsonl_path.open("w", encoding="utf-8")

    def note(text: str) -> None:
        """Nur in die Log-Datei, nicht auf den Bildschirm."""
        if log_file is not None:
            log_file.write(text + "\n")
            log_file.flush()  # Rohdaten sofort sichern, nicht erst am Ende

    def emit(text: str) -> None:
        print(text)
        note(text)

    def on_subscribe(sub: rawlog.Subscription) -> None:
        emit(cli.format_subscription(sub))

    first_frame = True

    def on_frame(frame: rawlog.RawFrame) -> None:
        nonlocal first_frame
        if first_frame:
            first_frame = False
            emit("\n" + "-" * 72)
        emit(cli.format_frame(frame))
        if jsonl_file is not None:
            jsonl_file.write(cli.frame_as_json(frame) + "\n")
            jsonl_file.flush()

    print()
    print("Verbinde und abonniere Notifications ...")
    print("Es wird kein Telegramm gesendet - nur mitgehoert.")
    if args.seconds > 0:
        print(f"Aufzeichnung: {args.seconds:.0f} s (Ctrl-C beendet frueher)")
    else:
        print("Aufzeichnung: unbegrenzt - Ctrl-C beendet.")
    print()

    emit(
        cli.format_listen_header(
            address=address, name=device_name, seconds=args.seconds
        )
    )

    try:
        result = await rawlog.listen(
            connect_target,
            seconds=args.seconds,
            connect_timeout=args.timeout,
            char_uuids=args.notify_uuid,
            on_frame=on_frame,
            on_subscribe=on_subscribe,
            stop_event=stop,
        )
        if result.name is None:
            result.name = device_name
        emit(cli.format_listen_summary(result))
    except BaseException as exc:
        # Ohne diesen Vermerk bleibt eine Datei mit reinem Kopf zurueck, die
        # aussieht wie "300 s gemessen, nichts empfangen" - obwohl der
        # Verbindungsaufbau gescheitert ist. Ausgabe macht cli.run.
        note("")
        note("-" * 72)
        note(f"ABBRUCH - keine gueltige Messung: {type(exc).__name__}: {exc}")
        raise
    finally:
        for handle in (log_file, jsonl_file):
            if handle is not None:
                handle.close()

    if log_path is not None:
        print()
        print(f"Roh-Log:  {log_path}")
        print(f"JSONL:    {jsonl_path}")

    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return cli.run(lambda: run_listen(args), debug=args.debug)


if __name__ == "__main__":
    sys.exit(main())
