"""Phase 7 - passiver Roh-Logger fuer BLE-Notifications.

Verbindet sich, abonniert alle Notify-/Indicate-Characteristics und schreibt
mit, was das Geraet von selbst sendet. Es wird **kein Telegramm gesendet** -
weder Auth noch Read noch irgendein Konfigurationszugriff.

Einzige Ausnahme, bewusst und benannt
-------------------------------------
``start_notify`` schreibt das **CCCD** (0x2902) der jeweiligen Characteristic.
Das ist der vom Bluetooth-Standard vorgesehene Weg, ein Abonnement zu
aktivieren; es adressiert nicht das BMS-Protokoll und veraendert keinen
BMS-Parameter. Der Sicherheitsgrundsatz in ``__init__.py`` erlaubt genau das
("Aktivieren von Notifications"). ``gatt.py`` schreibt weiterhin nichts.

Ob dieses Geraet ohne Anfrage ueberhaupt etwas sendet, ist offen - das ist der
Zweck dieses Moduls: messen statt annehmen.
"""

from __future__ import annotations

import asyncio
import datetime as _dt
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field

from bleak import BleakClient
from bleak.backends.device import BLEDevice

from . import uuids

DEFAULT_LISTEN_SECONDS = 60.0

#: Richtungskennungen im Log. ``TX`` existiert in Phase 7 nicht und ist nur
#: vergeben, damit ein spaeterer aktiver Logger dasselbe Format nutzen kann.
RX = "RX"
TX = "TX"


@dataclass(frozen=True)
class RawFrame:
    """Ein einzelnes empfangenes Notification-Paket.

    ``offset`` ist die Zeit seit Beginn des Abonnements (monotone Uhr) - fuer
    die Analyse von Sendeintervallen brauchbarer als die Wanduhr.
    """

    seq: int
    offset: float
    iso: str
    direction: str
    char_uuid: str
    handle: int | None
    data: bytes

    @property
    def short_uuid(self) -> str:
        return uuids.short(self.char_uuid)


@dataclass(frozen=True)
class Subscription:
    """Ergebnis eines ``start_notify``-Versuchs pro Characteristic."""

    uuid: str
    handle: int | None
    properties: tuple[str, ...]
    active: bool
    error: str | None = None

    @property
    def short_uuid(self) -> str:
        return uuids.short(self.uuid)


@dataclass
class ListenResult:
    address: str
    name: str | None
    seconds_requested: float
    elapsed: float = 0.0
    subscriptions: tuple[Subscription, ...] = ()
    frames: list[RawFrame] = field(default_factory=list)
    disconnected: bool = False
    stopped_by_user: bool = False

    @property
    def active_subscriptions(self) -> tuple[Subscription, ...]:
        return tuple(s for s in self.subscriptions if s.active)

    def frames_by_char(self) -> dict[str, list[RawFrame]]:
        grouped: dict[str, list[RawFrame]] = {}
        for frame in self.frames:
            grouped.setdefault(frame.short_uuid, []).append(frame)
        return grouped


def _properties(obj: object) -> tuple[str, ...]:
    return tuple(getattr(obj, "properties", ()) or ())


def _subscribable(props: Sequence[str]) -> bool:
    return "notify" in props or "indicate" in props


async def listen(
    target: str | BLEDevice,
    *,
    seconds: float = DEFAULT_LISTEN_SECONDS,
    connect_timeout: float = 20.0,
    char_uuids: Sequence[str] | None = None,
    on_frame: Callable[[RawFrame], None] | None = None,
    on_subscribe: Callable[[Subscription], None] | None = None,
    stop_event: asyncio.Event | None = None,
) -> ListenResult:
    """Abonniert Notifications und sammelt alles, was hereinkommt.

    ``seconds``     Laufzeit; ``<= 0`` bedeutet unbegrenzt (bis ``stop_event``
                    gesetzt oder die Verbindung abbricht).
    ``char_uuids``  beschraenkt das Abonnement auf diese UUIDs.
    ``on_frame``    wird pro Paket aufgerufen - fuer Live-Ausgabe und fuer das
                    sofortige Wegschreiben in eine Datei.
    ``stop_event``  sauberer Abbruch von aussen (z. B. SIGINT-Handler).

    Ausnahmen aus ``on_frame`` werden verworfen: eine kaputte Ausgabe darf eine
    laufende Aufzeichnung nicht abbrechen.
    """
    wanted = {uuids.normalize(u) for u in char_uuids} if char_uuids else None

    result = ListenResult(
        address=target if isinstance(target, str) else target.address,
        name=None if isinstance(target, str) else getattr(target, "name", None),
        seconds_requested=seconds,
    )

    loop = asyncio.get_running_loop()
    lost = asyncio.Event()

    def on_disconnect(_client: object) -> None:
        loop.call_soon_threadsafe(lost.set)

    started = time.monotonic()
    counter = 0

    def handler(sender: object, data: bytearray) -> None:
        nonlocal counter
        counter += 1
        frame = RawFrame(
            seq=counter,
            offset=time.monotonic() - started,
            iso=_dt.datetime.now().isoformat(timespec="milliseconds"),
            direction=RX,
            char_uuid=str(getattr(sender, "uuid", sender)),
            handle=getattr(sender, "handle", None),
            data=bytes(data),
        )
        result.frames.append(frame)
        if on_frame is not None:
            try:
                on_frame(frame)
            except Exception:  # noqa: BLE001 - Ausgabe darf Capture nicht toeten
                pass

    async with BleakClient(
        target, timeout=connect_timeout, disconnected_callback=on_disconnect
    ) as client:
        subscriptions: list[Subscription] = []
        started = time.monotonic()

        for service in client.services:
            for char in service.characteristics:
                props = _properties(char)
                if not _subscribable(props):
                    continue
                if wanted is not None and uuids.normalize(str(char.uuid)) not in wanted:
                    continue

                error: str | None = None
                try:
                    await client.start_notify(char, handler)
                except Exception as exc:  # ein Abo darf einzeln scheitern
                    error = f"{type(exc).__name__}: {exc}"

                sub = Subscription(
                    uuid=str(char.uuid),
                    handle=getattr(char, "handle", None),
                    properties=props,
                    active=error is None,
                    error=error,
                )
                subscriptions.append(sub)
                if on_subscribe is not None:
                    on_subscribe(sub)

        result.subscriptions = tuple(subscriptions)

        if result.active_subscriptions:
            await _wait(seconds, lost=lost, stop_event=stop_event)

        result.elapsed = time.monotonic() - started
        result.disconnected = lost.is_set()
        result.stopped_by_user = bool(stop_event and stop_event.is_set())

        # Abmelden ist Hoeflichkeit, kein Muss - Fehler hier sind irrelevant,
        # die Rohdaten stehen bereits im Ergebnis.
        if not result.disconnected:
            for sub in result.active_subscriptions:
                try:
                    await client.stop_notify(sub.uuid)
                except Exception:  # noqa: BLE001
                    pass

        if result.name is None:
            result.name = getattr(client, "name", None)

    return result


async def _wait(
    seconds: float,
    *,
    lost: asyncio.Event,
    stop_event: asyncio.Event | None,
) -> None:
    """Wartet auf Ablauf der Zeit, Verbindungsverlust oder Abbruch."""
    waiters = [asyncio.ensure_future(lost.wait())]
    if stop_event is not None:
        waiters.append(asyncio.ensure_future(stop_event.wait()))

    timeout = seconds if seconds > 0 else None
    try:
        await asyncio.wait(
            waiters, timeout=timeout, return_when=asyncio.FIRST_COMPLETED
        )
    finally:
        for waiter in waiters:
            waiter.cancel()
