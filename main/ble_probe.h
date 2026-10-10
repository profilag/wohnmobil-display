/**
 * @file ble_probe.h
 * @brief GATT-Erkunder: Dienste und Merkmale eines Geräts sichtbar machen.
 *
 * Gebaut für den Akku mit HumsiENK-BMS, dessen Protokoll nirgends öffentlich
 * dokumentiert ist. Was das Advertising hergibt, ist ausgereizt — der einzige
 * Weg weiter führt über eine Verbindung.
 *
 * ## Was er tut
 *
 * Verbinden, **alle** Dienste und Merkmale auflisten, sich auf jedes Merkmal
 * mit Notify- oder Indicate-Eigenschaft anmelden und anschließend jedes
 * lesbare Merkmal einmal auslesen. Viele BMS fangen nach der Anmeldung von
 * allein an zu senden; andere legen ihre Messwerte einfach in lesbare Merkmale.
 * Beides landet im Mitschnitt und ist der Ausgangspunkt für alles Weitere.
 *
 * Die Reihenfolge ist Absicht: erst anmelden, dann lesen — so geht eine
 * Antwort, die durch das Lesen ausgelöst wird, nicht verloren.
 *
 * ## Was er ausdrücklich nicht tut
 *
 * **Er schreibt nichts in ein Merkmal.** Bei einem unbekannten BMS kann ein
 * Schreibzugriff Grenzwerte, Kalibrierung oder Schaltzustände verstellen — das
 * ist kein Risiko, das man beim Erkunden eingeht. Geschrieben wird einzig das
 * CCCD, also der Anmeldeschalter für Benachrichtigungen; das ist der
 * standardisierte Weg und verändert am Gerät nichts.
 *
 * Damit kann dieses Modul ein Protokoll **beobachten**, aber keins auslösen,
 * das erst auf eine Anfrage antwortet. Bleibt der Mitschnitt leer, ist genau
 * das der Befund — und dann führt kein Weg daran vorbei, den Verkehr der
 * HumsiENK-App mitzuschneiden.
 *
 * ## Nur einer gleichzeitig
 *
 * Wie beim LED-Streifen gilt: solange das Board verbunden ist, kommt die
 * Hersteller-App nicht an den Akku. Nachgemessen: Während das Board verbunden
 * ist, **wirbt das BMS überhaupt nicht mehr** — ein Mac-Client findet es dann
 * nicht einmal. Das Modul kann also genau eine Verbindung.
 *
 * ## Warum es lange nicht ging: der fehlende ATT-Server
 *
 * Wochenlang schrieb das Board alle Kommandos, das BMS quittierte jedes
 * einzelne auf ATT-Ebene — und es kam nie eine Benachrichtigung an. Ein
 * Byte-für-Byte-Vergleich mit dem PacketLogger-Mitschnitt der Hersteller-App
 * zeigte **keinen einzigen Unterschied**: gleiche Dienst- und Merkmalssuche,
 * CCCD 0x0009 = 02 00 und 0x000F = 01 00 in derselben Reihenfolge, dann Write
 * Request auf Handle 12. Eine Gegenprobe vom Mac (CoreBluetooth, dieselben
 * Kommandos) holte die echten Werte — am Protokoll lag es also nicht.
 *
 * Der Fehler saß in unserer eigenen Konfiguration. NimBLE koppelt den
 * ATT-Server an die Peripheral-Rolle, und in der Empfangstabelle von
 * `ble_att.c` steht
 *
 *     #if MYNEWT_VAL(BLE_GATTS)
 *         { BLE_ATT_OP_NOTIFY_REQ, ble_att_svr_rx_notify },
 *
 * Eine **eingehende** Benachrichtigung ist auf ATT-Ebene eine Anfrage an den
 * Server. Ohne Server fehlt der Eintrag für Opcode 0x1B, und jedes Paket wird
 * verworfen, bevor es die Anwendung sieht — auch beim reinen Client. Mit
 * CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n war das Board also taub, obwohl es
 * verbunden war und alles andere funktionierte. Siehe sdkconfig.defaults.
 *
 * Was auf dem Weg dorthin ausgeschlossen wurde und **nicht** die Ursache war:
 * ATT-MTU (251 wie voreingestellt, 185 wie iPhone und Mac), 2M-/Coded-PHY,
 * eigene Adresse (öffentlich gegen gewürfelte Zufallsadresse),
 * Verbindungsparameter (feste 30 ms und 720 ms wie das iPhone),
 * Schreibmodus (Write Request gegen Write Command), gleichzeitiger Scan.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Voreingestellte Adresse BLE_PROBE_DEFAULT_MAC (der Akku aus der
 * HumsiENK-App) steht in secrets.h. */
#include "secrets_select.h"

#define BLE_PROBE_MAX_SVCS    12
#define BLE_PROBE_MAX_ENTRIES 32
#define BLE_PROBE_UUID_LEN    40
/** Suchdauer, bevor aufgegeben wird. */
#define BLE_PROBE_SEARCH_MS   10000

typedef enum {
    BLE_PROBE_IDLE = 0,
    BLE_PROBE_SEARCHING,
    BLE_PROBE_CONNECTING,
    BLE_PROBE_DISCOVERING,
    BLE_PROBE_SUBSCRIBING,
    BLE_PROBE_READY,
    BLE_PROBE_FAILED,
} ble_probe_state_t;

typedef struct {
    char svc[BLE_PROBE_UUID_LEN]; /*!< UUID des Dienstes als Text  */
    char chr[BLE_PROBE_UUID_LEN]; /*!< UUID des Merkmals als Text  */
    uint16_t val_handle;
    uint16_t svc_end;     /*!< letztes Handle des Dienstes — Suchbereich */
    uint16_t cccd_handle; /*!< per Deskriptorsuche gefunden, 0 = keins */
    uint8_t props;        /*!< BLE_GATT_CHR_PROP_* */
    bool subscribed;      /*!< CCCD erfolgreich geschrieben */
} ble_probe_entry_t;

/* --- API ----------------------------------------------------------------- */

esp_err_t ble_probe_init(void);

/**
 * @brief Gerät suchen und verbinden.
 *
 * Gesucht wird zuerst im Funk, um den Adresstyp sicher zu erwischen — ihn zu
 * raten führt bei zufälligen Adressen zu Verbindungsversuchen ins Leere.
 *
 * @param mac "AA:BB:CC:DD:EE:FF", Anzeigereihenfolge wie in der App
 */
esp_err_t ble_probe_connect(const char *mac);

esp_err_t ble_probe_disconnect(void);

ble_probe_state_t ble_probe_get_state(void);
const char *ble_probe_state_text(ble_probe_state_t state);

/** @brief Kopie der gefundenen Merkmale. @return Anzahl Einträge */
size_t ble_probe_snapshot(ble_probe_entry_t *out, size_t max);

/* --- Anfragen senden ------------------------------------------------------ */

/**
 * @brief Rohbytes an das beschreibbare Merkmal schicken.
 *
 * @warning **Das ist der gefährliche Teil.** Solange das Protokoll unbekannt
 *          ist, weiß niemand, was eine Bytefolge im BMS auslöst — sie kann
 *          ebenso gut Schutzgrenzen verstellen wie einen Messwert abfragen.
 *          Hier gehören nur Bytes hinein, deren Bedeutung belegt ist: aus dem
 *          Mitschnitt der Hersteller-App. Geraten wird hier nicht.
 *
 * Das Zielmerkmal wird bei der Erkundung bestimmt — das erste mit Schreibrecht.
 * Gesendete Bytes stehen als `W @Handle:` im Mitschnitt, direkt vor der Antwort.
 *
 * @param hex Hexbytes als Text, Trennzeichen und `0x` sind erlaubt:
 *            `"DD A5 03 00 FF FD 77"`
 */
esp_err_t ble_probe_write_hex(const char *hex);

/** @brief Handle des Schreibmerkmals, 0 wenn keins gefunden wurde. */
uint16_t ble_probe_write_handle(void);

/* --- Messwerte des Akkus -------------------------------------------------- */

/** So viele Zellspannungen werden gemerkt; das Protokoll sieht 24 vor. */
#define BLE_PROBE_CELL_MAX 8

/**
 * Dekodierte Werte aus den Antworten 0x21, 0x22 und 0x20.
 *
 * Die Feldaufteilung ist **belegt**, nicht vermutet: Sie stammt aus einem
 * Mitschnitt der Hersteller-App und ist an vier echten Antworten geprüft —
 * Prüfsumme stimmig, und die Summe der vier Zellspannungen (13,854 V) deckt
 * sich mit der getrennt übertragenen Packspannung (13,850 V).
 */
typedef struct {
    bool valid;

    uint32_t voltage_mv;
    int32_t current_ma; /*!< positiv = Laden */
    uint8_t soc;        /*!< % */
    uint8_t soh;        /*!< % */
    uint32_t remaining_mah;
    uint32_t design_mah;
    uint16_t cycles;
    int8_t temp[6]; /*!< °C */

    uint16_t cell_mv[BLE_PROBE_CELL_MAX];
    uint8_t cell_count;

    uint32_t status;
    bool charge_mos;
    bool discharge_mos;
    bool balancing;

    char version[16];
} ble_probe_battery_t;

/** @brief Kopie der zuletzt empfangenen Messwerte. */
void ble_probe_battery(ble_probe_battery_t *out);

/** @brief Alter der Messwerte in ms, -1 wenn noch keine da sind. */
int64_t ble_probe_battery_age_ms(void);

/**
 * @brief Zeitstempel des letzten Datensatzes (esp_timer-Mikrosekunden).
 *
 * Wer mitschreibt, muss neue Datensätze von wiederholt gelesenen unterscheiden
 * können. Das Alter allein taugt dafür nicht — es ändert sich ständig.
 *
 * @return negativ, solange nie etwas ankam.
 */
int64_t ble_probe_battery_stamp_us(void);

/* --- Rahmen-Hypothese ----------------------------------------------------- */

/**
 * @brief Vermuteter Rahmenaufbau des HumsiENK-BMS.
 *
 * ```
 * AA │ CMD │ LEN │ Nutzdaten … │ CK_LO │ CK_HI
 * ```
 * Prüfsumme: 16-Bit-Summe über CMD, LEN und Nutzdaten, little-endian abgelegt.
 *
 * @warning **Unbestätigt.** Für HumsiENK existiert keine öffentliche
 *          Protokolldokumentation; dieser Aufbau stammt aus einer Vorgabe ohne
 *          nachprüfbare Quelle. Die drei Beispielbefehle sind in sich schlüssig
 *          (`0x20 + 0x00 = 0x20`), mehr sagt das nicht.
 *
 * Der Rahmenleser behandelt ihn deshalb als Hypothese: Er protokolliert jeden
 * Rahmen mit empfangener **und** berechneter Prüfsumme, verwirft aber nichts.
 * Passt die Prüfsumme durchgehend, ist die Hypothese bestätigt; passt sie nie,
 * ist der Aufbau ein anderer — und der Mitschnitt zeigt trotzdem alles.
 */
#define BLE_PROBE_FRAME_HEADER 0xAA

/**
 * @brief Die drei Leseabfragen der Reihe nach senden.
 *
 * `AA 21 00 21 00` (Batteriedaten), dann `AA 22 00 22 00` (Zellspannungen),
 * dann `AA 20 00 20 00` (Status), mit Abstand dazwischen. Weiter geschaltet
 * wird in `ble_probe_tick()`, damit nichts blockiert.
 *
 * Steht die Verbindung noch nicht, werden die Abfragen **vorgemerkt** und
 * gesendet, sobald sie steht — der Aufbau dauert mehrere Sekunden, und einen
 * Tastendruck in dieser Zeit zu verwerfen wäre nur ärgerlich.
 *
 * @warning Diese Kommandos sind Teil derselben unbestätigten Hypothese. Sie
 *          sind als Leseabfragen *gemeint* — garantiert ist das nicht, solange
 *          das Protokoll unbekannt ist.
 */
esp_err_t ble_probe_start_queries(void);

/** @brief true, solange die Abfragefolge läuft oder vorgemerkt ist. */
bool ble_probe_queries_running(void);

/** @brief true, wenn die Abfragen auf die fertige Verbindung warten. */
bool ble_probe_queries_pending(void);

/* --- Betrieb -------------------------------------------------------------- */

/**
 * @brief Regelmäßig aus dem UI-Task aufrufen (etwa alle 500 ms).
 *
 * Erledigt zwei Dinge, die nicht in einen BLE-Rückruf gehören, weil sie warten
 * müssen: das Weiterschalten der Abfragefolge und den Wiederverbindungsversuch
 * nach einem Abbruch.
 */
void ble_probe_tick(void);

/** @brief true, wenn die Verbindung gewollt ist — steuert den Reconnect. */
bool ble_probe_wants_connection(void);

/**
 * @brief Einmalige vollständige Attributauflistung, danach trennen.
 *
 * Die reguläre Erkundung sucht **primäre** Dienste (`0x2800`). Ein als
 * **sekundär** deklarierter Dienst (`0x2801`) taucht dabei nicht auf, ebenso
 * wenig Attribute jenseits des letzten gefundenen Dienstes. Genau das ist der
 * Verdacht bei diesem Akku: Er bewirbt `0xFF00`, aber die Dienstsuche findet
 * ihn nicht.
 *
 * Diese Funktion fragt deshalb mit `Find Information` **jedes** Attribut von
 * Handle 1 bis 0xFFFF ab und liest anschließend alle Deklarationen aus, um
 * die dahinterliegenden UUIDs aufzulösen. Rein lesend.
 *
 * Nach dem Durchlauf trennt sie von allein — der Akku gehört danach wieder
 * der Hersteller-App.
 */
esp_err_t ble_probe_survey_once(void);

/**
 * @brief Handle des gefundenen CCCD am ersten Notify-Merkmal, 0 = keins.
 *
 * Gehört auf den Schirm, nicht nur ins Log: Ob die Anmeldung auf
 * Benachrichtigungen überhaupt zustande kam, ist die Voraussetzung für alles
 * Weitere — und im Protokoll geht diese eine Zeile zwischen Hunderten unter.
 */
uint16_t ble_probe_notify_cccd(void);

/** @brief true, wenn die Anmeldung auf dieses CCCD bestätigt wurde. */
bool ble_probe_notify_subscribed(void);

/**
 * @brief Verschlüsselung/Kopplung anstoßen.
 *
 * @warning Das ist der einzige Schritt, der am BMS etwas **dauerhaft**
 *          hinterlässt: einen Kopplungsschlüssel. Manche Geräte haben nur
 *          wenige Plätze dafür und werfen den ältesten hinaus — im
 *          ungünstigsten Fall muss die Hersteller-App neu gekoppelt werden.
 *
 * Der Verdacht dahinter: Das BMS erzwingt Verschlüsselung nicht auf ATT-Ebene
 * (Lesen wird mit „Read Not Permitted" abgelehnt, nicht mit „Insufficient
 * Authentication"), könnte aber auf Anwendungsebene stumm bleiben, solange die
 * Verbindung unverschlüsselt ist. Ein Telefon koppelt beiläufig, unser Board
 * bisher nie.
 */
esp_err_t ble_probe_pair(void);

/* --- Feldsuche ------------------------------------------------------------ */

/**
 * @brief Ein bekannter Messwert und seine möglichen Zahlendarstellungen.
 *
 * Der Trick beim Dekodieren eines unbekannten Rahmens: man liest die Werte in
 * der Hersteller-App ab und sucht sie in den Rohbytes. Steht die Spannung dort
 * als `05 46`, ist das Feld gefunden und die Einheit gleich mit — 1350 in
 * Hundertstel Volt.
 *
 * Jede Antwort wird automatisch danach durchsucht; Treffer stehen im
 * Mitschnitt unter der Zeile mit den Rohbytes.
 *
 * Die Tabelle steht in `ble_probe.c` und ist beim Ablesen neuer Werte dort
 * anzupassen. Je weiter die Werte auseinanderliegen, desto eindeutiger die
 * Treffer — ein Wert bei vollem und einer bei halbem Akku sagt mehr als zwei
 * Ablesungen im selben Zustand.
 */
typedef struct {
    const char *name; /*!< "Spannung 13,5 V" */
    const char *unit; /*!< "0,01 V" — die Einheit, die zu diesem Zahlwert passt */
    uint32_t value;   /*!< der gesuchte Zahlwert */
} ble_probe_hint_t;

/** @brief Zähler, der bei jeder Änderung der Liste hochgeht. */
uint32_t ble_probe_generation(void);

/** @brief Eigenschaften als Kürzel, z. B. "R Wn N". `buf` braucht 16 Byte. */
void ble_probe_props_text(uint8_t props, char *buf, size_t len);

/* --- Mitschnitt der Benachrichtigungen ----------------------------------- */

/** @brief Bisher empfangene Benachrichtigungen. */
uint32_t ble_probe_notify_count(void);

/** @brief Mitschnitt als Text kopieren. @return geschriebene Länge */
size_t ble_probe_log_text(char *buf, size_t len);

/** @brief Zähler, der bei jeder neuen Zeile hochgeht. */
uint32_t ble_probe_log_generation(void);

void ble_probe_log_clear(void);

#ifdef __cplusplus
}
#endif
