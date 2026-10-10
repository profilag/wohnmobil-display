# Waveshare ESP32-S3-Touch-LCD-7 — ESP-IDF-Projekt

Natives ESP-IDF-5.5-Projekt für das 7"-Touch-Board, inklusive LVGL 9, GT911-Touch,
CH422G-On-Board-Expander und dem externen **AW9523B**-IO-Modul am gleichen I2C-Bus.

## Schnellstart

```bash
cd ~/esp/projects/ws-s3-lcd7

./tools/idf.sh build                     # bauen
./tools/idf.sh -p /dev/cu.usbmodem5A7A0329631 flash monitor
```

Verifizierte Hardware dieses Exemplars: ESP32-S3 (QFN56) rev v0.2, 8 MB oktaler
PSRAM, 16 MB Flash (eFuse: quad), MAC `AA:BB:CC:DD:EE:FF` (anonymisiert).

Erwartetes Boot-Log (auf dem Board verifiziert):

```
I (829)  bsp: I2C0 bereit (SCL=9, SDA=8, 400 kHz)
I (829)  ch422g: CH422G initialisiert (IO0..IO7 = Ausgang)
I (989)  bsp: RGB-Panel 800x480 @ 16 MHz aktiv
I (1300) GT911: TouchPad_ID:0x39,0x31,0x31
I (1303) bsp: GT911-Touch bereit
I (1372) bsp: LVGL 9.5.0 bereit
I (1445) aw9523b: AW9523B auf 0x58 erkannt (ID 0x23, P0 Push-Pull, IMAX 0)
I (1454) bsp:   0x20…0x3F: CH422G (24 Adressen, unvollständige Dekodierung …)
I (1468) bsp:   0x58 — AW9523B
I (1469) bsp:   0x5D — GT911 Touch
I (1587) app: freier Heap: 249607 B intern, 6847948 B PSRAM
```

In VS Code: `⇧⌘B` baut, weitere Tasks über **Terminal → Run Task…**
(Flash + Monitor, Menuconfig, Size, Erase Flash, Ports auflisten).
Alternativ die Buttons der ESP-IDF-Extension in der Statuszeile.

Monitor verlassen: `Ctrl-]`

## Wichtig: Python-Umgebung

Das System-`python3` ist inzwischen **3.14**, ESP-IDF 5.5.1 hat aber nur ein venv
für 3.12. Ein nacktes `source ~/esp/esp-idf/export.sh` bricht deshalb mit
*"virtual environment … not found"* ab. Lösungen:

* `./tools/idf.sh <befehl>` — setzt `IDF_PYTHON_ENV_PATH` selbst (empfohlen)
* `source tools/env.sh` — für ein interaktives Terminal
* Die VS-Code-Extension ist nicht betroffen: sie liest ihre Pfade aus
  `~/.espressif/esp_idf.json`, wo bereits das 3.12-venv eingetragen ist.
  Zusätzlich setzt `.vscode/settings.json` `IDF_PYTHON_ENV_PATH` über
  `idf.customExtraVars`.

## Hardware-Übersicht

### Display — ST7262, 800 × 480, RGB565 parallel (16 Bit)

| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| HSYNC | 46 | VSYNC | 3 |
| DE | 5 | PCLK | 7 |
| B0–B4 | 14, 38, 18, 17, 10 | G0–G5 | 39, 0, 45, 48, 47, 21 |
| R0–R4 | 1, 2, 42, 41, 40 | DISP | — (nicht belegt) |

PCLK 16 MHz, fallende Flanke. Timing: HPW 4 / HBP 8 / HFP 8, VPW 4 / VBP 8 / VFP 8.
Framebuffer liegt im PSRAM, die DMA speist sich aus einem **Bounce Buffer** im
internen RAM — das ist die von Espressif empfohlene Gegenmaßnahme gegen das
bekannte Wegdriften des Bildes beim ESP32-S3.

### I2C — ein Bus für alles

`SCL = GPIO9`, `SDA = GPIO8`, 400 kHz, `I2C_NUM_0`

| Adresse | Baustein |
|---|---|
| 0x20 / 0x23 / 0x24 / 0x26 / 0x38 | CH422G (nutzt mehrere feste Adressen) |
| 0x5D (Reserve 0x14) | GT911 Touch |
| 0x58 … 0x5B | AW9523B (extern, per AD0/AD1 wählbar) |

`bsp_i2c_scan()` listet beim Start alles auf, was am Bus antwortet — der erste
Anlaufpunkt, wenn das AW9523B-Modul nicht erkannt wird.

### CH422G (On-Board-Expander)

| Pin | Funktion |
|---|---|
| EXIO1 | Touch-Reset (TP_RST) |
| EXIO2 | LCD-Backlight ein/aus |
| EXIO3 | LCD-Reset |
| EXIO4 | SD-Karte Chip-Select |
| EXIO5 | **USB_SEL**: low = USB, high = CAN |

Der CH422G ist ein Sonderfall: er hat keinen Registerzeiger, sondern vier feste
I2C-Adressen, an die je ein Datenbyte geschrieben wird. Seine Ausgangsregister
sind nicht rücklesbar, deshalb hält der Treiber den Zustand im RAM.

Backlight lässt sich nur **schalten**, nicht dimmen — der Pin hängt am Expander,
nicht an einem LEDC-Kanal.

### AW9523B (externes IO-Modul)

16 IOs: `AW9523B_P0(0..7)` und `AW9523B_P1(0..7)`. Jeder Pin kann entweder GPIO
(Ein-/Ausgang) oder Konstantstrom-LED-Ausgang mit 256 Helligkeitsstufen sein.

Zwei Fallen, die der Treiber dokumentiert:

* **Port 0 ist per Reset Open-Drain.** Für echte High-Pegel `p0_push_pull = true`
  setzen (die Standardkonfiguration tut das schon). Port 1 ist immer Push-Pull.
* Der **INT-Ausgang** ist active-low und wird erst durch Lesen der
  Input-Register quittiert — `aw9523b_read_all()` erledigt das.

```c
aw9523b_handle_t aw = bsp_aw9523b_get_handle();

aw9523b_set_direction(aw, AW9523B_P1(0), GPIO_MODE_OUTPUT);
aw9523b_set_level(aw, AW9523B_P1(0), true);

aw9523b_set_led_mode(aw, AW9523B_P0(0), true);      // Konstantstromsenke
aw9523b_set_brightness(aw, AW9523B_P0(0), 128);     // 0…255

uint16_t inputs;
aw9523b_read_all(aw, &inputs);                      // quittiert auch den INT
```

Fehlt das Modul, läuft das Board trotzdem hoch: `bsp_init()` protokolliert eine
Warnung und `bsp_aw9523b_get_handle()` liefert `NULL`.

## Übernommene Funktionen aus `~/workspace/Moin`

### Display-Beleuchtung ein/aus

```c
bsp_display_sleep(BSP_DISPLAY_WAKE_LOCKOUT_MS);  // aus, 2 s Sperrzeit
bsp_display_wake();                              // sofort an
bsp_backlight_set(true);                         // roh, ohne Sperrlogik
bool an = bsp_backlight_is_on();
```

Die **Sperrzeit ist der Kern der Sache**: der GT911 prellt nach, und die
Berührung, die den „Display aus"-Button auslöst, würde das Display sonst
unmittelbar wieder aufwecken. Nach Ablauf der Sperrzeit weckt jede Berührung
des Bildschirms die Beleuchtung wieder auf — der Handler hängt am
Eingabegerät, nicht an einem Widget, denn bei dunklem Display sieht man nicht,
wo die Bedienelemente sind.

Bei dunklem Display gehört die **gesamte Touchfläche allein dem Einschalten**:
die weckende Berührung wird im Handler verbraucht (`lv_indev_stop_processing`
plus `lv_indev_wait_release`) und erreicht kein Bedienelement mehr. Sonst
schaltet der blinde Griff nebenbei PA0/PA1 oder den Pegel-Test.

### Lastausgänge auf dem AW9523B

| Ausgang | Funktion |
|---|---|
| **PA0** | Relais Wasserversorgung aus dem Frischwassertank |
| **PA1** | Relais Heizung des Abwassertanks |
| **PA2**, **PA3** | frei (trugen früher die Anregung, die jetzt auf Port B liegt) |

```c
bsp_output_set(BSP_OUTPUT_PA0, true);    // Last an
bsp_output_set(BSP_OUTPUT_PA1, false);   // Last aus
bool an = bsp_output_get(BSP_OUTPUT_PA0);
```

`bsp_output_init()` wird von `bsp_init()` automatisch aufgerufen und richtet
PA0–PA3 ein. Zwei Eigenschaften sind aus der alten Firmware bewusst
übernommen, weil die Verdrahtung darauf beruht:

* **Low-aktiv** (`BSP_OUTPUT_ACTIVE_LOW`): „an" bedeutet Low-Pegel, der Ausgang
  zieht die Last nach Masse. Ruhezustand ist high.
* **Port A bleibt Open Drain** (`GCR` Bit 4 = 0, Reset-Vorgabe des AW9523B).
  Auf Push-Pull umzustellen würde gegen einen etwaigen externen Pull-up
  arbeiten — deshalb steht `p0_push_pull = false` in `BSP_CONFIG_DEFAULT()`.

Beim Hochlauf wird zuerst der inaktive Pegel ins Ausgangsregister geschrieben
und erst danach auf Ausgang umgeschaltet, damit die Last nicht kurz anzappelt.

Auf dem Board verifiziert (Rücklesung der AW9523B-Register):

| Zustand | `OUT_P0` |
|---|---|
| alles aus | `0x07` |
| PA0 an | `0x06` |
| PA1 an | `0x05` |

`GCR = 0x00` (Open Drain). `CONFIG_P0 = 0xF0`, weil PA0–PA3 als Ausgänge
eingerichtet werden — PA2 und PA3 sind zwar unbenutzt, bleiben aber definiert
auf ihrem Ruhepegel, statt offen zu schweben.

### Namensschema PA/PB

Das alte Projekt nennt die AW9523B-Ports **PA** und **PB**, das Datenblatt und
dieser Treiber nennen sie **P0** und **P1**. `PA0` ist also `AW9523B_P0(0)`,
`PB3` ist `AW9523B_P1(3)`.

## Wasserstandsmessung Frischwassertank

### Pin-Belegung

| Funktion | AW9523B | Richtung | Bemerkung |
|---|---|---|---|
| Anregung / Bezugselektrode | **PB0** | Ausgang | low-aktiv, Push-Pull |
| Elektrode unten (tiefste) | **PB1** | Eingang | externer Pull-up nötig |
| Elektrode mitte | **PB2** | Eingang | externer Pull-up nötig |
| Elektrode oben (kürzeste) | **PB3** | Eingang | externer Pull-up nötig |

Anregung und Sensoren liegen auf demselben Port — ein Kabelbaum statt zweier.
**Port 1 des AW9523B ist immer Push-Pull**, anders als Port 0: Ein Ausgang
treibt hier aktiv auf High, statt nur nach Masse zu ziehen. Für die Anregung
ist das richtig, denn im Ruhezustand liegt die Bezugselektrode damit auf
demselben Pegel wie die Pull-ups der Sensoren — kein Potentialunterschied,
kein Strom, keine Elektrolyse.

PA0/PA1 bleiben für die Relais frei. Alle vier Pins sind Konstanten in
[`main/water_level.h`](main/water_level.h) und dort in einer Zeile umzustellen.

### Externe Pull-ups sind Pflicht

Der AW9523B hat **keine internen Pull-up-Widerstände**. Jede Elektrodenleitung
braucht einen Pull-up von typisch 10 kΩ nach 3V3:

```
        3V3
         │
        10k
         │
PB1 ─────┼───────── Elektrode unten ┐
                                     │  Wasser leitet
PB0 ─────────────── Bezugselektrode ─┘
   (zieht bei Messung auf GND)
```

Ohne Wasserkontakt hält der Pull-up den Eingang auf High = trocken. Berührt das
Wasser die Elektrode, zieht die bestromte Bezugselektrode den Eingang auf Low =
nass. Diese Polarität steht in `WATER_ELECTRODE_WET_LEVEL`.

**Ohne Pull-ups liest der Baustein alle Eingänge als Low** — die Anzeige würde
dauerhaft „voll" melden. Genau das zeigte der Test bei unbeschalteten Eingängen
(`PortB=0x00`). Der Pegel-Test auf dem Schirm macht das sofort sichtbar.

### Elektrolyse

Der Messkreis wird **nur während einer Messung** bestromt: Anregung an,
40 ms Beruhigung, drei Abtastungen mit 8 ms Abstand, Anregung aus. Gemessene
Einschaltdauer: **55 ms pro Messung**. Ausgelöst wird ausschließlich per
Knopfdruck, es gibt keine automatische Wiederholung. Dazwischen liegt der
Messkreis spannungsfrei.

Der Pegel-Test durchbricht das absichtlich, um die Pegel live beobachten zu
können — er schaltet sich nach `WATER_TESTMODE_TIMEOUT_MS` (30 s) selbst ab.
Die Firmware summiert die gesamte Bestromungszeit und schreibt sie ins Log.

### Vierstufige Anzeige

Drei Elektroden ergeben genau vier unterscheidbare Zustände:

| Nasse Elektroden | Anzeige | Prozent |
|---|---|---|
| keine | leer / Reserve | 0 |
| unten | 1/3 voll | 33 |
| unten + mitte | 2/3 voll | 66 |
| alle drei | voll | 100 |

### Plausibilitätsprüfung

Eine nasse Elektrode über einer trockenen ist physikalisch unmöglich und deutet
auf Leitungsbruch, Korrosion oder eine vertauschte Leitung hin. Der Dekoder
zählt deshalb nur den lückenlosen Teil von unten und setzt zusätzlich das Flag
`implausible`; die Oberfläche zeigt dann eine Warnung. Zu niedrig anzuzeigen ist
die harmlosere Fehlerrichtung als ein Tank, der voller wirkt als er ist.

Wahrheitstabelle, auf dem Board verifiziert (Bit = 1 heißt trocken):

| `PortB` | Anzeige | `implausible` |
|---|---|---|
| `0x07` | leer / Reserve | – |
| `0x06` | 1/3 voll | – |
| `0x04` | 2/3 voll | – |
| `0x00` | voll | – |
| `0x03` | leer / Reserve | ja |
| `0x05` | leer / Reserve | ja |
| `0x01` | leer / Reserve | ja |
| `0x02` | 1/3 voll | ja |

## Abwassertank

Der Abwassertank hat **nur eine Elektrode**: „voll" oder „nicht voll",
Zwischenstände gibt es nicht.

### Pin-Belegung

| Funktion | AW9523B | Richtung | Bemerkung |
|---|---|---|---|
| Anregung / Bezugselektrode | **PB5** | Ausgang | low-aktiv, Push-Pull |
| Elektrode „voll" | **PB6** | Eingang | externer Pull-up nötig |

Beide Konstanten stehen in [`main/waste_level.h`](main/waste_level.h).

**Warum ein eigener Anregungsausgang und nicht PB0?** Weil die Abwasserprüfung
im Gegensatz zur Frischwassermessung von allein läuft. Läge sie auf PB0, würden
die Frischwasserelektroden alle fünf Minuten mitbestromt — genau das, was die
Frischwasserlogik vermeidet.

Misst die Elektrode statt gegen eine Bezugselektrode direkt gegen Tankwand oder
Fahrzeugmasse, bleibt PB5 einfach unbeschaltet. Die Messung funktioniert dann
unverändert, nur ohne die schonende Taktung.

### Zyklische Prüfung

Geprüft wird alle `WASTE_CHECK_INTERVAL_MS` (**5 Minuten**) und zusätzlich über
den Knopf „prüfen". Eine Handprüfung setzt den Takt zurück. Pro Prüfung liegt
die Anregung rund 70 ms an — dieselbe Sequenz wie beim Frischwasser (40 ms
Beruhigung, drei Abtastungen mit 8 ms Abstand, Mehrheitsentscheid). Bei
5 Minuten Takt sind das etwa **0,02 % Einschaltdauer**.

Das Log meldet nur **Zustandswechsel** auf Info-Ebene; der unveränderte Fall
geht nach Debug, sonst würde der Takt das Log zuschreiben.

Läuft gerade der Abwasser-Pegel-Test, setzt die zyklische Prüfung aus — sie
würde die Dauer-Anregung am Ende abschalten und den Test stillschweigend
beenden. Beim Beenden des Tests wird sofort nachgeprüft.

## Die beiden Seiten der Oberfläche

**Betrieb** — alles für den Alltag: Frischwasserstand mit Messknopf, Abwasser
mit Zustandsbalken und Prüfknopf, sowie die Knöpfe *Wasser* (PA0), *Heizung*
(PA1) und *Display aus*.

**Test** — die Rohpegel beider Messkreise, je mit einem Schalter für die
dauerhafte Anregung. Bewusst getrennt: der Testmodus bestromt die Elektroden
dauerhaft und fördert Elektrolyse, das gehört nicht neben die täglichen
Bedienelemente. Beide Schalter fallen nach 30 s von allein zurück.

**Licht** — Steuerung des LED-Streifens, siehe eigenen Abschnitt.

**Akku** — GATT-Erkunder für das BMS, siehe eigenen Abschnitt.

**Funk** — BLE-Scanner, siehe eigenen Abschnitt weiter unten.

Die Pegelanzeige läuft immer mit — Lesen ist passiv und braucht nur einen
einzigen Port-B-Zugriff für beide Tanks. Nur die Anregung hängt an den
Schaltern. Ohne Anregung meldet eine Elektrode zwangsläufig „trocken"; die
Betriebsseite zeigt deshalb weiter den zyklisch gemessenen Wert und wechselt
nur im Testmodus auf den Live-Pegel.

## BLE-Scanner (Seite „Funk")

Vorarbeit für zwei mögliche Datenquellen: einen **Victron-Solarregler** und
einen **Akku mit Bluetooth-BMS**. Der Scanner beantwortet drei Fragen, die man
vor jeder Anbindung klären muss: Was ist überhaupt in Reichweite, wie stark
kommt es am Einbauort an, und welches Protokoll spricht es vermutlich.

Der Schalter startet und stoppt, die Tabelle zeigt Name, Adresse, Feldstärke
(aktuell und bester Wert seit dem Leeren), die erkannte Geräteart und die
beworbenen Dienste. Sortiert wird nach Feldstärke, das Nächstliegende steht
oben. Jedes Gerät wird außerdem einmal ins Log geschrieben, sobald sein Name
bekannt ist — bequemer als vom Schirm abzutippen.

Die **Dienste**-Spalte ist der Schlüssel für ein unbekanntes BMS: dort steht
meist genau der Dienst, über den später die Daten laufen. `FF00` spricht für
JBD, `FFE0` für einen HM-10-artigen UART-Dienst, eine 128-Bit-UUID für etwas
Herstellereigenes. Das kostet nichts, weil es im Advertising steht — keine
Verbindung nötig.

### Was er tut und was nicht

Er **hört zu und verbindet sich mit nichts** — keine GATT-Verbindung, kein
Pairing. Damit stört er auch keine Handy-App, die gerade mit demselben Gerät
spricht. Das ist kein Detail: die meisten BMS lassen nur **einen** Client
gleichzeitig zu, ein verbundenes Board würde die Hersteller-App aussperren.

Gescannt wird allerdings **aktiv**, das Board sendet also Scan-Requests. Ohne
die kommt keine Scan-Response, und ohne Scan-Response bleiben viele Geräte
namenlos — gerade der Name ist hier die gesuchte Information.

### Erkennung

| Anzeige | Grundlage | Verlässlichkeit |
|---|---|---|
| `Victron` | Hersteller-ID `0x02E1` in den Manufacturer Data | sicher |
| `JBD/Xiaoxiang?` | Name beginnt mit `xiaoxiang` oder `JBD` | Vermutung |
| `JK-BMS?` | Name beginnt mit `JK_` oder `JK-` | Vermutung |
| `Daly?` | Name beginnt mit `DL-` oder `DALY` | Vermutung |
| `Hersteller 0x…` | Manufacturer Data ohne bekannte Kennung | – |

Die BMS-Erkennung ist eine Namensheuristik. Die Hersteller liefern ihre Module
unter wechselnden Handelsmarken aus — der Advertising-Name bleibt aber meist
der des OEM, und genau daran hängt die Wahl des Protokolls.

### Funk neben dem RGB-Panel

Das Panel schaufelt dauernd per DMA aus dem PSRAM; alles, was den Cache anhält,
wird als Zucken oder Wegdriften sichtbar. Drei Vorkehrungen dagegen:

* **NimBLE startet erst beim ersten Scan.** Wer die Seite nie öffnet, hat ein
  Board, das exakt so läuft wie ohne das Modul — der Bootvorgang ist unverändert.
* **Bonding ist abgeschaltet** (`CONFIG_BT_NIMBLE_NVS_PERSIST=n`). Ohne
  Verbindungen gibt es nichts zu speichern, also auch keine NVS-Schreibzugriffe,
  die den Cache stillegen.
* **Tastverhältnis rund 30 %** (60 ms Fenster alle 200 ms) statt Dauerempfang.

Zeigt das Bild trotzdem Störungen, sind `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` und
`CONFIG_SPIRAM_RODATA` die nächsten Stellschrauben — sie verlagern Code und
Konstanten ins PSRAM, sodass Flash-Zugriffe den Bildaufbau nicht mehr anhalten.
Beide sind bewusst **aus**, weil sie das gesamte Laufzeitverhalten ändern und
ohne beobachtetes Problem nicht zu rechtfertigen sind.

Nur der Beobachter-Rolle ist einkompiliert; Central, Peripheral und Broadcaster
sind in `sdkconfig.defaults` abgeschaltet.

### Wie es weitergeht

Für den **Victron-Regler** wäre der nächste Schritt, die Manufacturer Data des
`0x02E1`-Geräts mit dem Schlüssel aus VictronConnect (Settings → Product Info →
Instant Readout via Bluetooth → Show) per AES-CTR zu entschlüsseln. Das bleibt
passiv. Die Alternative ohne Funk ist **VE.Direct** am MPPT-Port: vollständiger
Datensatz, Klartextprotokoll, kein Schlüssel — GPIO 15/16/43/44 sind frei.

Für den **Akku** hängt alles am erkannten BMS-Typ. Anders als bei Victron
braucht es dafür eine echte GATT-Verbindung über einen UART-artigen Dienst, und
damit die oben genannte Einschränkung: entweder das Board oder die App.

## LED-Streifen (Seite „Licht")

Steuert einen **ELK-BLEDOM** — den billigen chinesischen BLE-Controller, der
auch als `MELK-…`, `LEDBLE` oder `XROCKER` verkauft wird. Anders als der
Scanner ist [`main/led_ble.c`](main/led_ble.c) ein echter GATT-Client: er
verbindet sich.

### Bedienung

| Element | Wirkung |
|---|---|
| Schalter oben | Ein / Aus |
| Verbinden / Trennen | Sucht 10 s nach `ELK-BLEDOM…` und verbindet |
| Helligkeit | 0…100 % |
| Weiß: warm ↔ kalt | Farbtemperatur, warm und kalt ergänzen sich zu 100 |
| Rot / Grün / Blau | Feste Farbe, Vorschaubalken folgt sofort |
| Farbfelder | Acht Voreinstellungen |
| Effekt | Alle 30 IDs `0x80`…`0x9D` |
| Tempo | Effektgeschwindigkeit 0…100 |

Solange nichts verbunden ist, sind alle Bedienelemente gesperrt.

### Protokoll

Dienst `0xFFF0`, Schreib-Charakteristik `0xFFF3`, ohne Antwort beschrieben.
Neun Byte je Befehl, `0x7E` vorn, `0xEF` hinten — die vollständige Tabelle
steht im Kopf von [`main/led_ble.h`](main/led_ble.h). Findet sich `0xFFF3`
nicht, nimmt der Client das erste beschreibbare Merkmal des Dienstes; das
deckt abweichende Baureihen ab, ohne dass man den Quelltext anfassen muss.

Die **Effektnamen sind nicht verlässlich dokumentiert** und unterscheiden sich
zwischen Baureihen. Deshalb sind alle 30 IDs durchnummeriert erreichbar und nur
die vier belegten tragen einen Namen — Ausprobieren ist hier schneller als
Suchen.

### Zwei Eigenheiten, die man kennen muss

**Der Controller meldet nichts zurück.** Was die Oberfläche zeigt, ist ein
Schattenzustand aus dem, was zuletzt gesendet wurde. Nach einem Stromausfall am
Streifen kann er von der Wirklichkeit abweichen.

**Nur ein Client gleichzeitig.** Solange das Board verbunden ist, kommt die
Hersteller-App nicht an den Streifen. Deshalb wird die Verbindung ausdrücklich
auf- und abgebaut und nie von allein wiederhergestellt.

Schieberegler werden auf einen Befehl je 120 ms gedrosselt; beim Loslassen geht
der Endwert in jedem Fall raus. Ungebremst kommt der Controller nicht mit und
die Verbindung bricht ab.

### Zusammenspiel mit dem Scanner

Der BLE-Controller kann nicht gleichzeitig suchen und verbinden. Ein laufender
Scan auf der Funk-Seite wird beim Verbinden deshalb automatisch beendet und der
Schalter dort zurückgesetzt.

Beide Module teilen sich den Stack über
[`main/ble_host.c`](main/ble_host.c) — `nimble_port_init()` darf nur einmal
laufen. Wer die Bereitmeldung braucht, trägt sich dort **vor** dem ersten
`ble_host_start()` ein; damit ist die Rückrufliste fertig, wenn der Host
synchronisiert.

## Solarladeregler (Seite „Solar")

Liest den Victron SmartSolar über **Instant Readout** mit — rein passiv, aus
dem Advertising. Keine Verbindung, kein Pairing, kein Verbindungslimit: die
VictronConnect-App läuft ungestört weiter. Das ist der Unterschied zum
LED-Streifen und zum Akku, wo immer nur einer darf.

Angezeigt werden PV-Leistung, Batteriespannung, Ladestrom, Tagesertrag,
Ladezustand und Fehler. Der Schalter auf der Seite ist **derselbe Scan** wie
auf der Funk-Seite, nur anders beschriftet — beide Schalter ziehen sich
gegenseitig nach.

### Aufbau des Pakets

```
E1 02 │ 10 02 │ 57 A0 │ 01 │ B9 0C │ 49 │ 06 8E A2 …
  │       │       │      │     │      │     └ AES-CTR-Nutzlast, 12 Byte
  │       │       │      │     │      └ Prüfbyte = erstes Byte des Schlüssels
  │       │       │      │     └ Zähler (LE) = CTR-Nonce, +1 je Paket
  │       │       │      └ Satzart 0x01 = Solarladeregler
  │       │       └ Modell-ID (LE), hier 0xA057
  │       └ „Product Advertisement"
  └ Hersteller 0x02E1 = Victron
```

AES-128 im CTR-Modus. Der Zählerblock ist der 16-Bit-Wert little-endian in den
ersten zwei Byte, Rest null. Weil die Nutzlast mit zwölf Byte kürzer als ein
AES-Block ist, wird nie hochgezählt — die Zählrichtung spielt keine Rolle.

Dass der Zähler bei jedem Paket steigt, ist auch der Grund, warum dieselben
Messwerte jedes Mal völlig anders verschlüsselt aussehen.

Die Feldaufteilung der entschlüsselten zwölf Byte steht im Kopf von
[`main/victron_ble.h`](main/victron_ble.h). Jedes Feld hat einen eigenen Wert
für „liegt nicht vor" (`0x7FFF`, `0xFFFF`, `0x1FF`); die werden abgefangen,
sonst stünde etwa `51,1 A` am unbenutzten Lastausgang.

### Der Schlüssel

Gerätespezifisch, in VictronConnect unter Zahnrad → Produktinfo → „Instant
Readout via Bluetooth" → Anzeigen. Steht als `VICTRON_KEY_HEX` in
[`main/victron_ble.h`](main/victron_ble.h). Sein erstes Byte muss mit dem
Prüfbyte im Paket übereinstimmen — passt es nicht, zählt die Seite die
verworfenen Pakete und sagt es. Beim Zurücksetzen des Reglers ändert er sich.

Er gibt nur Lesezugriff auf Telemetrie, ist also kein Generalschlüssel zum
Regler — trotzdem gehört er behandelt wie ein Zugangsdatum.

## Akku: GATT-Erkunder (Seite „Akku")

Der Akku hat ein **HumsiENK**-BMS, dessen Protokoll nirgends öffentlich
dokumentiert ist. Weder [batmon-ha](https://github.com/fl4p/batmon-ha) noch
[BMS_BLE-HA](https://github.com/patman15/BMS_BLE-HA) kennen es, der
Advertising-Name `HS00AC` passt auf kein bekanntes Muster, und in den
Manufacturer Data steht nur Apples Kennung `0x004C` — ein iBeacon-Rahmen, wie
ihn billige BLE-Module gern mitkopieren. Dass `HS00AC` tatsächlich der Akku
ist, verrät die MAC `XX:XX:XX:XX:00:AC`: die letzten beiden Bytes stehen im
Namen.

Damit ist der passive Weg zu Ende, und [`main/ble_probe.c`](main/ble_probe.c)
geht den nächsten: verbinden, **alle** Dienste und Merkmale auflisten, sich auf
jedes Merkmal mit Notify oder Indicate anmelden und danach jedes lesbare
Merkmal einmal auslesen. Viele BMS senden nach der Anmeldung von allein; andere
legen ihre Messwerte einfach in lesbare Merkmale. Erst anmelden, dann lesen —
so geht eine Antwort, die das Lesen auslöst, nicht verloren.

Gefundene Dienste und Merkmale gehen zusätzlich ins Log. Vom Schirm abzutippen
ist fehleranfällig, und eine 128-Bit-UUID passt dort nur knapp in die Spalte.

### Was er ausdrücklich nicht tut

**Er schreibt in kein Merkmal.** Bei einem unbekannten BMS kann ein
Schreibzugriff Grenzwerte, Kalibrierung oder Schaltzustände verstellen — kein
Risiko, das man beim Erkunden eingeht. Geschrieben wird einzig das CCCD, also
der standardisierte Anmeldeschalter für Benachrichtigungen.

Die Kehrseite: ein Protokoll, das erst **auf eine Anfrage antwortet**, lässt
sich so nicht auslösen. Bleibt der Mitschnitt leer, ist genau das der Befund —
und dann führt kein Weg daran vorbei, den Verkehr der HumsiENK-App
mitzuschneiden.

### Feldsuche

Der Trick beim Dekodieren eines unbekannten Rahmens: Werte in der
Hersteller-App ablesen und sie in den Rohbytes suchen. Steht die Spannung dort
als `05 46`, ist das Feld gefunden und die Einheit gleich mit — 1350 in
Hundertstel Volt.

Das macht der Erkunder automatisch. Jede Antwort wird gegen die abgelesenen
Werte geprüft, in allen gängigen Skalierungen und in beiden Byte-Reihenfolgen:

```
N @14: 55 AA 03 05 46 00 3C 16 64 ...
   @3  05 46  BE  Spannung 13,5 V  [0,01 V]
   @5  00 3C  BE  Strom 6 A        [0,1 A]
   @7  16         Temperatur 22 °C [°C]
   @8  64         Ladung 100 %     [%]
```

Die Tabelle steht als `s_hints` in [`main/ble_probe.c`](main/ble_probe.c) und
ist beim Ablesen neuer Werte dort anzupassen. Einzelne Bytes treffen oft
zufällig — erst ein Treffer, der über mehrere Antworten hinweg an derselben
Stelle steht und sich mit dem Messwert ändert, ist ein echtes Feld. Deshalb
steht der Versatz dabei. Und deshalb sind zwei Ablesungen bei **verschiedenem**
Ladezustand mehr wert als zehn beim selben.

### Anzeige

Oben die Merkmalstabelle: Dienst-UUID, Merkmal-UUID, Rechte (`R` lesen,
`W` schreiben, `Wn` ohne Antwort, `N` Notify, `I` Indicate) und das Handle;
ein Haken markiert die erfolgreiche Anmeldung. Unten der Mitschnitt als
`Handle: Bytes`, die ältesten Zeilen fallen bei vollem Puffer weg. Dieselben
Zeilen gehen ins Log, sind also über den Monitor kopierbar.

Die Adresse steht als `BLE_PROBE_DEFAULT_MAC` in
[`main/ble_probe.h`](main/ble_probe.h). Gesucht wird sie erst im Funk und dann
verbunden — den Adresstyp zu raten führt bei zufälligen Adressen wie dieser zu
Verbindungsversuchen ins Leere.

Das CCCD wird auf `Wert-Handle + 1` vermutet, statt je Merkmal eine eigene
Deskriptor-Suche zu fahren. Wo die Annahme nicht stimmt, schlägt genau diese
eine Anmeldung fehl und der Rest läuft weiter.

## Zwei Fallen bei der Textausgabe

Beide kosten einen Flash-Durchgang, wenn man sie nicht kennt.

**LVGLs `printf` beherrscht kein `%f`.** `lv_label_set_text_fmt(l, "%.2f V", x)`
zeigt nicht `13,50 V`, sondern ein einsames `f` — der Rest des Formats
verschwindet, und es gibt weder Warnung noch Fehler. Deshalb bleiben die
Victron-Messwerte durchweg ganzzahlig in ihrer Protokollskalierung, und die
Ausgabe läuft über `tile_set_fixed()` mit `snprintf` aus der C-Bibliothek.
Nebeneffekt: das Dezimalkomma ist deutsch, was `%f` nie geliefert hätte.

**Der Font kennt nur, was einkompiliert wurde.** Siehe unten — jedes Zeichen
über `0x7F` muss im erzeugten Bereich stehen, sonst erscheint ein leerer
Kasten.

Beide Fehler sehen im Quelltext richtig aus und fallen erst auf dem Schirm auf.

## Schrift: warum eigene Fonts

Die mitgelieferten `lv_font_montserrat_*` sind mit `-r 0x20-0x7F,0xB0,0x2022`
erzeugt — **ohne Umlaute**, ohne Gedankenstrich, ohne Ellipse. In einer
deutschen Oberfläche wird daraus „pr□fen" und „Ger□te"; LVGL zeichnet für jedes
fehlende Zeichen einen leeren Kasten.

`main/fonts/font_ui_{14,20,28}.c` sind deshalb aus derselben
`Montserrat-Medium.ttf` und derselben FontAwesome-Auswahl erzeugt wie die
eingebauten, nur zusätzlich mit **0xC0–0xFF** (Latin-1, enthält ÄÖÜäöüß) sowie
en-Dash, em-Dash und Ellipse. Sie sind damit ein direkter Ersatz, die
`LV_SYMBOL_*`-Zeichen funktionieren unverändert. Kosten: rund 25 kB Flash.

Gesetzt werden sie an zwei Stellen: als Grundschrift auf dem Bildschirm (davon
erbt alles, was keine eigene setzt) und ausdrücklich auf `LV_PART_ITEMS` der
BLE-Tabelle, denn Tabellenzellen erben die Schrift nicht.

Der Befehl zum Neuerzeugen steht im Kopf von
[`main/fonts/fonts_ui.h`](main/fonts/fonts_ui.h). Wichtig dabei: die
FontAwesome-Symbolliste aus dem Kopfkommentar von `lv_font_montserrat_14.c`
übernehmen, sonst fehlen die Symbole.

## Projektstruktur

```
CMakeLists.txt              Projektwurzel
sdkconfig.defaults          Quelle der Wahrheit für die Konfiguration
partitions.csv              2 × 4 MB OTA + 4 MB SPIFFS in 16 MB Flash
main/main.c                 Oberfläche: Seiten "Betrieb" und "Test"
main/water_level.{c,h}      Frischwasser, drei Elektroden, Messung auf Knopfdruck
main/waste_level.{c,h}      Abwasser, eine Elektrode, Prüfung alle 5 Minuten
main/ble_host.{c,h}         gemeinsamer NimBLE-Stack für Scanner und LED-Client
main/ble_scan.{c,h}         BLE-Beobachter: Victron- und BMS-Erkennung
main/led_ble.{c,h}          GATT-Client für den ELK-BLEDOM-Streifen
main/ble_probe.{c,h}        GATT-Erkunder für das unbekannte Akku-BMS
main/victron_ble.{c,h}      Victron Instant Readout entschlüsseln (AES-CTR)
main/fonts/                 Montserrat mit Umlauten (die eingebauten können keine)
components/bsp_ws_lcd7/     Board-Init: I2C, Expander, RGB-Panel, Touch, LVGL
components/ch422g/          Treiber On-Board-Expander
components/aw9523b/         Treiber externes IO-Modul
tools/idf.sh                idf.py-Wrapper mit korrekter Python-Umgebung
tools/env.sh                für `source tools/env.sh` im Terminal
```

## LVGL benutzen

Der LVGL-Task läuft in `esp_lvgl_port`. Jeder `lv_*`-Aufruf von außerhalb muss
den Mutex halten:

```c
if (bsp_display_lock(0)) {          // 0 = unbegrenzt warten
    lv_label_set_text(label, "Hallo");
    bsp_display_unlock();
}
```

Innerhalb von LVGL-Event-Callbacks ist der Mutex bereits gehalten — dort **nicht**
erneut sperren, das führt zum Deadlock.

FPS- und CPU-Anzeige ist über `CONFIG_LV_USE_PERF_MONITOR` eingeschaltet
(setzt `CONFIG_LV_USE_SYSMON` voraus — beides steht in `sdkconfig.defaults`).

## Konfiguration ändern

`sdkconfig` wird generiert und gehört nicht ins Git. Dauerhafte Änderungen in
`sdkconfig.defaults` eintragen, dann:

```bash
rm -f sdkconfig && ./tools/idf.sh reconfigure
```

`./tools/idf.sh menuconfig` ist gut zum Ausprobieren, die Änderung landet aber
nur in `sdkconfig` und ist beim nächsten Löschen weg.

## Debugging

Der ESP32-S3 hat JTAG eingebaut — es ist keine externe Probe nötig, die
Haupt-USB-C-Buchse genügt. Die Launch-Konfiguration
*"ESP-IDF: Debug (USB-JTAG)"* nutzt `board/esp32s3-builtin.cfg`.

## Bekannte Stolperstellen

* **Zwei USB-C-Buchsen.** Nur die **"USB TO UART"**-Buchse (CH343P, `1a86:55d3`)
  liefert hier einen Port: `/dev/cu.usbmodem5A7A0329631`. Der CH343P ist
  CDC-konform, deshalb heißt der Port `usbmodem…` und nicht `wchusbserial…` —
  der installierte WCH-VCP-Treiber wird gar nicht gebraucht. An der Haupt-Buchse
  (nativer USB-Serial/JTAG) erschien kein Port; vermutlich steht EXIO5 nach dem
  Kaltstart auf high, also im CAN-Modus. Zum Flashen und Loggen ist die
  UART-Buchse der zuverlässige Weg.
* Logs gehen bewusst auf beide Wege: UART0 als primäre Konsole (→ CH343P-Buchse),
  USB-Serial/JTAG als sekundäre.
* **Kein Port sichtbar?** Datenkabel statt Ladekabel, direkt am Mac statt am Hub,
  dann `ls /dev/cu.*`. Achtung bei zsh: `ls /dev/cu.usbmodem*` bricht bei
  Nichttreffer das ganze Kommando ab — besser `ls /dev | grep cu\\.`
* **GT911-Adresse.** Der Touch entscheidet beim Reset anhand des INT-Pegels über
  seine Adresse (INT low → 0x5D). Deshalb treibt `bsp_touch_reset()` GPIO4 vor
  dem Reset-Puls aktiv low und gibt ihn danach wieder frei. Diese Reihenfolge
  nicht umstellen.
* **PSRAM ist Pflicht.** Ohne oktalen PSRAM passt der 800 × 480-Framebuffer
  (768 kB) nicht in den Speicher.
* **`direct_mode` und `avoid_tearing` sind in `esp_lvgl_port` gekoppelt.** Der
  Flush-Callback nimmt bei einem RGB-Panel im `direct_mode` das Semaphor
  `trans_sem`, angelegt wird das aber nur im `avoid_tearing`-Zweig
  (`esp_lvgl_port_disp.c:379` gegen `:754`). Die Kombination `direct_mode = true`
  mit `avoid_tearing = false` stürzt daher beim ersten Bildaufbau ab
  (`assert failed: xQueueSemaphoreTake queue.c:1709`). `bsp_lvgl_init()` leitet
  `direct_mode` deshalb aus `avoid_tearing` ab — nicht wieder entkoppeln.
* **I2C-Scan im Bereich 0x20–0x3F.** Der CH422G dekodiert seine Adresse
  unvollständig und quittiert auf allen 24 Adressen dieses Bereichs. Ein eigener
  Baustein dort (z. B. ein SSD1306 auf 0x3C) ist am Bus nicht von ihm zu
  unterscheiden — solche Peripherie besser über den AW9523B anbinden oder auf
  einen zweiten I2C-Port legen.

## Referenz

* [Waveshare Wiki ESP32-S3-Touch-LCD-7](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7)
* [Waveshare Docs](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7)
* [ESP-IDF RGB-LCD](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html)
* [esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port)
