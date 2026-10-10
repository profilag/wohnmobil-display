# HumsiENK BLE – Read-Only-Client (macOS)

Werkzeuge zur Untersuchung des Bluetooth-LE-Protokolls eines
**HumsiENK 12 V 314 Ah LiFePO4-Akkus** und – sobald das Protokoll bestätigt ist –
ein schlanker Read-Only-Monitor.

Sprache: Python 3, BLE über [bleak](https://github.com/hbldh/bleak) (nutzt auf
macOS CoreBluetooth).

---

## ⚠️ Sicherheitsgrundsatz

Dieses Projekt arbeitet **ausschließlich lesend**.

Es werden **keine** BMS-Konfigurationsparameter verändert – insbesondere nicht:

Ladeparameter · Entladeparameter · Zellspannungsgrenzen · Überstromgrenzen ·
Temperaturgrenzen · Balancing · MOSFET-Zustände · BMS-Abschaltung ·
Kapazitätswerte · Kalibrierwerte · Passwort-/Security-Einstellungen

BLE-Schreibzugriffe sind nur zulässig, wenn sie zwingend erforderlich sind für

1. einen **bekannten** Auth-/Unlock-Handshake oder
2. eine **reine Datenabfrage**.

Unbekannte Telegramme werden nicht geschrieben, solange ihr Zweck nicht geklärt ist.

**Aktueller Stand (Phase 1–3, 7):** Es wird kein Telegramm gesendet.
`scan.py` verbindet sich nicht einmal; `inspect_gatt.py` verbindet sich, führt
Discovery durch und liest optional lesbare Characteristics (reine Abfrage);
`listen.py` abonniert Notifications und hört zu.

Einzige Ausnahme, bewusst und benannt: `listen.py` schreibt über `start_notify`
das **CCCD** (`0x2902`) der abonnierten Characteristic. Das ist der vom
Bluetooth-Standard vorgesehene Weg, ein Abonnement einzuschalten – es adressiert
nicht das BMS-Protokoll und verändert keinen BMS-Parameter. `gatt.py` schreibt
weiterhin nichts, auch keine CCCD.

---

## Installation (macOS)

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### Bluetooth-Berechtigung

macOS verlangt eine explizite Freigabe pro App:

```
Systemeinstellungen
→ Datenschutz & Sicherheit
→ Bluetooth
```

Dort muss **Terminal** bzw. **Visual Studio Code** aktiviert sein – also die App,
aus der Python gestartet wird. Nach dem Aktivieren die App **komplett neu starten**.

Fehlt die Berechtigung, findet der Scan typischerweise *keine* Geräte oder bleak
meldet einen Autorisierungsfehler.

### Smartphone-App trennen

Die originale **HumsiENK-App** auf dem Smartphone sollte geschlossen bzw. dort
Bluetooth getrennt sein. Viele BMS erlauben nur **eine aktive BLE-Verbindung** –
solange das Handy verbunden ist, ist der Akku für den Mac oft unsichtbar oder
nicht verbindbar.

---

## Benutzung

### Scan (Phase 2)

```bash
python scripts/scan.py
```

Optionen:

```bash
python scripts/scan.py --seconds 20            # länger scannen
python scripts/scan.py --prefix HS --prefix Humsi
python scripts/scan.py --no-capture            # nichts in captures/ schreiben
python scripts/scan.py --debug                 # Tracebacks
```

Ausgabe pro Gerät: Name, CoreBluetooth-UUID, RSSI, TX-Power, advertised Service
UUIDs, Manufacturer Data, Service Data. Geräte mit Namenspräfix `HS` werden mit
`>>>` hervorgehoben.

> **macOS-Hinweis:** Bleak liefert unter macOS **keine Bluetooth-MAC-Adresse**,
> sondern eine CoreBluetooth-Peripheral-UUID. Diese ist Mac-lokal (kann sich
> zwischen Rechnern und teils nach Neupairing unterscheiden), taugt aber als
> Verbindungsziel für `--address`.

### GATT untersuchen (Phase 3)

```bash
python scripts/inspect_gatt.py
```

Optionen:

```bash
python scripts/inspect_gatt.py --read                    # lesbare Werte mitlesen
python scripts/inspect_gatt.py --address <CB-UUID>       # Scan überspringen
python scripts/inspect_gatt.py --prefix Humsi --seconds 20
python scripts/inspect_gatt.py --timeout 30 --debug
```

Werden mehrere passende Geräte gefunden, erscheint eine Auswahlliste.
Pro Characteristic werden UUID, Handle, Properties, Descriptors sowie
Read-/Write-/Notify-/Indicate-Fähigkeit ausgegeben.

Am Ende erfolgt ein rein **deskriptiver** Abgleich gegen die bei anderen
HumsiENK-Geräten dokumentierten UUIDs `FFF0` / `FFF1` / `FFF2` / `FFFA` –
ohne daraus schon eine Protokollentscheidung abzuleiten.

### Passiv mithören (Phase 7)

```bash
python scripts/listen.py --address <CB-UUID>
```

Abonniert alle Notify-/Indicate-Characteristics und schreibt mit, was das Gerät
**von selbst** sendet. Es wird kein Telegramm gesendet – nur zugehört. Manche BMS
pushen nach dem Subscribe zyklisch Statusframes; ob dieses Gerät das tut, ist die
Frage, die dieses Skript beantwortet.

Optionen:

```bash
python scripts/listen.py --seconds 300         # länger aufzeichnen
python scripts/listen.py --seconds 0           # unbegrenzt, bis Ctrl-C
python scripts/listen.py --notify-uuid 0003    # nur diese Characteristic
python scripts/listen.py --no-capture          # nichts in captures/ schreiben
```

Jedes Paket wird **sofort** in die Log-Datei geschrieben, nicht erst am Ende.
Ctrl-C beendet geordnet und schreibt noch die Zusammenfassung – Rohdaten gehen
dabei nicht verloren.

Pro Paket werden Sequenznummer, Zeitversatz seit Abo-Start, Richtung,
Characteristic, Länge, Hex und ASCII ausgegeben. Die Zusammenfassung nennt
Paketzahl pro Characteristic, Längenbereich, Sendeintervalle und wie viele
Pakete sich inhaltlich unterscheiden.

### Monitor (Phase 9 – noch nicht implementiert)

```bash
python scripts/monitor.py
```

Wird erst gebaut, wenn das Protokoll dieses konkreten Akkus anhand echter
Rohdaten bestätigt ist.

---

## Projektstruktur

```
.
├── README.md
├── requirements.txt
├── .gitignore
│
├── src/humsienk/
│   ├── __init__.py
│   ├── uuids.py       # UUID-Normalisierung, Kurzform, SIG-Namen
│   ├── scanner.py     # Phase 2: passiver BLE-Scan
│   ├── gatt.py        # Phase 3: GATT-Discovery
│   ├── rawlog.py      # Phase 7: passiver Notify-Logger
│   ├── cli.py         # Darstellung, Geräteauswahl, Fehler, Captures
│   ├── protocol.py    # Phase 4/5: Protokollerkennung + Handshake   (offen)
│   ├── crc.py         # Phase 6: CRC-16/MODBUS                      (offen)
│   └── parser.py      # Phase 8: Feld-Dekodierung                   (offen)
│
├── scripts/
│   ├── _bootstrap.py  # macht src/ importierbar
│   ├── scan.py
│   ├── inspect_gatt.py
│   ├── listen.py
│   └── monitor.py     # Phase 9                                     (offen)
│
├── tests/             # Phase 6/8
└── captures/          # Rohdaten – werden nie verworfen
```

Trennung der Schichten:

* `scanner.py` / `gatt.py` / `rawlog.py` – ausschließlich BLE-Transport
* `protocol.py` / `crc.py` / `parser.py` – ausschließlich Bytes, kein BLE
* `cli.py` / `scripts/` – ausschließlich Darstellung und Benutzerführung

---

## Captures

Alle Skripte schreiben ihren Report zusätzlich nach `captures/`:

```
captures/scan_2026-08-17_153211.txt
captures/gatt_2026-08-17_153244.txt
captures/raw_2026-08-17_155655.log      # lesbar
captures/raw_2026-08-17_155655.jsonl    # maschinenlesbar, für Phase 8
```

Der Roh-Logger schreibt beides parallel: das `.log` zum Lesen, das `.jsonl` mit
einem Objekt pro Paket (`seq`, `offset`, `iso`, `dir`, `uuid`, `handle`, `len`,
`hex`) als Eingabe für den späteren Parser.

`captures/` steht bewusst **nicht** in `.gitignore` – Rohdaten sind das
wertvollste Artefakt dieses Projekts.

---

## Roadmap

| Phase | Inhalt | Status |
|-------|--------------------------------------|--------|
| 1 | Projektstruktur | ✅ |
| 2 | BLE-Scanner | ✅ |
| 3 | GATT-Inspektor | ✅ |
| 4 | Protokollerkennung (`BmsProtocol`) | offen |
| 5 | HiLink-Handshake (nur bei eindeutiger Struktur) | offen |
| 6 | Frame-Protokoll + CRC-16/MODBUS + Tests | offen |
| 7 | Raw Capture (passiver Notify-Logger) | ✅ |
| 8 | Parser für Batteriedaten | offen |
| 9 | Live-Monitor | offen |
| 10 | Debug-Modus, Frame-Reassembly (`0x7E` … `0x0D`) | offen |
| 11 | Fehlerhandling | Grundstock in `cli.py` |
| 12 | README | ✅ |

---

## Gemessener Stand: HS00AC

Ergebnis von Phase 2 und 3 an diesem konkreten Akku
(`captures/scan_2026-08-17_154719.txt`, `captures/gatt_2026-08-17_154901.txt`):

```
Name:              HS00AC
Advertisement:     Services 0001 und FF00
                   Manufacturer 0x503A (4 B): 2A 51 00 AC

GATT (Discovery):  SERVICE 0001
                     CHAR 0002   write   (max 182 B, ohne Response)
                     CHAR 0003   notify  + CCCD
```

Bemerkenswert:

* Ein reines **Write/Notify-Paar**, kein einziges lesbares Characteristic.
  Kommandos gehen auf `0002`, Antworten kommen auf `0003`.
* Die dokumentierten HiLink-UUIDs `FFF0`/`FFF1`/`FFF2`/`FFFA` existieren hier
  **nicht**. Der unten beschriebene Ausgangshinweis betrifft eine andere
  BMS-Variante. Das sagt noch nichts über das *Frameformat* – Transport und
  Protokoll sind unabhängige Fragen.
* `FF00` wird beworben, erscheint aber nicht in der Discovery. Entweder nicht
  implementiert oder ein macOS-GATT-Cache-Effekt. `1800`/`1801` versteckt
  CoreBluetooth grundsätzlich.
* Phase 7 über **300 s: kein einziges Notification-Paket**
  (`captures/raw_2026-08-17_160154.log`). Das Abo auf `0003` war die ganze Zeit
  aktiv. Der Akku sendet also nichts von selbst – er ist reines
  Request/Response. Passives Mitlesen am Mac ist damit als Weg ausgeschlossen,
  gemessen und nicht vermutet.

## Nicht bestätigte Ausgangshinweise

Bei *anderen* HumsiENK-Geräten wurde ein WATT/HiLink-Protokoll beschrieben:

```
Service FFF0
  FFF1   notify
  FFF2   write
  FFFA   auth/unlock  ("HiLink" als ASCII)

Read-Frame (vermutet):
  7E 00 01 03 <addr:u16> <count:u16> <CRC16-MODBUS> 0D

Datenbereich um 0x008C soll Batteriedaten enthalten.
```

Das ist für **diesen** Akku **noch nicht bestätigt**. Es existieren
unterschiedliche HumsiENK-BMS-Varianten. Deshalb gilt: erst messen
(Phase 2/3), dann entscheiden (Phase 4) – keine Annahmen im Produktivcode.

---

## Fehlerbehandlung

Normale Benutzerfehler erscheinen als Klartextmeldung, nicht als Traceback.
Abgedeckt sind u. a.: Bluetooth aus, fehlende Berechtigung, Gerät nicht
gefunden, Gerät mit Smartphone verbunden, Verbindungsverlust, Timeout.
Tracebacks gibt es mit `--debug`.
