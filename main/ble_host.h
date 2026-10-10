/**
 * @file ble_host.h
 * @brief Gemeinsame Verwaltung des NimBLE-Hosts.
 *
 * Sowohl der Scanner (ble_scan.h) als auch der LED-Client (led_ble.h) brauchen
 * NimBLE, aber `nimble_port_init()` darf nur **einmal** laufen. Diese Schicht
 * besitzt den Stack, alle anderen fragen hier an.
 *
 * Hochgefahren wird weiterhin erst auf Anforderung: wer nie scannt und nie eine
 * Verbindung aufbaut, hat ein Board, das exakt so läuft wie ohne Bluetooth.
 *
 * ## Reihenfolge
 *
 * `ble_host_register()` gehört in die Initialisierung, **bevor** irgendwer
 * `ble_host_start()` ruft. Damit ist die Rückrufliste fertig, wenn der Host
 * synchronisiert — und es braucht keine Absicherung gegen einen Rückruf, der
 * mitten in der Registrierung eintrifft.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** So viele Module dürfen sich für die Bereitmeldung eintragen. */
#define BLE_HOST_MAX_CALLBACKS 4

/**
 * @brief Rückruf, sobald der Controller synchronisiert ist.
 *
 * @warning Läuft im NimBLE-Host-Task, nicht im LVGL-Task. Von hier aus keine
 *          `lv_*`-Funktionen aufrufen.
 */
typedef void (*ble_host_ready_cb_t)(void);

/** @brief Rückruf eintragen. Nur vor dem ersten `ble_host_start()` aufrufen. */
esp_err_t ble_host_register(ble_host_ready_cb_t cb);

/**
 * @brief NimBLE hochfahren, falls noch nicht geschehen. Mehrfach aufrufbar.
 *
 * Kehrt sofort zurück; die eingetragenen Rückrufe kommen erst, wenn der
 * Controller synchronisiert ist — typisch ein paar Dutzend Millisekunden.
 */
esp_err_t ble_host_start(void);

/** @brief true, sobald der Controller einsatzbereit ist. */
bool ble_host_is_ready(void);

/** @brief Eigener Adresstyp für GAP-Aufrufe. Erst nach `ble_host_is_ready()`. */
uint8_t ble_host_own_addr_type(void);

#ifdef __cplusplus
}
#endif
