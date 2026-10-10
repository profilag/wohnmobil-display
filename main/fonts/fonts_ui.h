/**
 * @file fonts_ui.h
 * @brief Oberflächen-Fonts mit deutschem Zeichensatz.
 *
 * ## Warum es die gibt
 *
 * Die mitgelieferten `lv_font_montserrat_*` sind mit `-r 0x20-0x7F,0xB0,0x2022`
 * erzeugt — also **ohne Umlaute**, ohne Gedankenstrich und ohne Ellipse. In
 * einer deutschsprachigen Oberfläche erscheint jedes dieser Zeichen als leerer
 * Kasten: aus „prüfen" wird „pr□fen", aus „—" ein einzelnes „□".
 *
 * Diese drei Fonts stammen aus derselben `Montserrat-Medium.ttf` und derselben
 * FontAwesome-Auswahl wie die eingebauten, sind also Zeichen für Zeichen
 * identisch — nur mit größerem Bereich:
 *
 * | Bereich       | wofür                                             |
 * |---------------|---------------------------------------------------|
 * | `0x20-0x7F`   | ASCII, wie gehabt                                 |
 * | `0xA0-0xFF`   | Latin-1: ÄÖÜäöüß, Mittelpunkt `·`, Grad `°`       |
 * | `0x2013/14`   | en-Dash und em-Dash                               |
 * | `0x2022`      | Bullet                                            |
 * | `0x2026`      | Ellipse `…`                                       |
 * | `0x2190-0x2194` | Pfeile, u. a. `↔`                               |
 *
 * Damit sind sie ein direkter Ersatz; die `LV_SYMBOL_*`-Zeichen funktionieren
 * unverändert. Kosten: rund 30 kB Flash für alle drei Größen zusammen.
 *
 * ## Wenn ein Kasten auftaucht
 *
 * Dann fehlt das Zeichen im Bereich. Entweder den Bereich unten erweitern und
 * neu erzeugen — oder, wenn es ein Symbol sein soll, ein `LV_SYMBOL_*` nehmen:
 * die liegen im FontAwesome-Teil und sind immer dabei. `LV_SYMBOL_OK` statt
 * `✓` etwa.
 *
 * Welche Zeichen die Oberfläche überhaupt braucht, verrät ein Blick auf alle
 * String-Literale — jedes Zeichen über `0x7F` muss im Bereich stehen.
 *
 * ## Neu erzeugen
 *
 * Braucht node; die TTF und die WOFF liegen in LVGL. Die Symbolliste ist die
 * aus dem Kopfkommentar von `lv_font_montserrat_14.c` — sie muss übernommen
 * werden, sonst fehlen die `LV_SYMBOL_*`-Zeichen.
 *
 * @code
 * npm install lv_font_conv
 * S=managed_components/lvgl__lvgl/scripts/built_in_font
 * ./node_modules/.bin/lv_font_conv --no-compress --no-prefilter --bpp 4 --size 14 \
 *     --font $S/Montserrat-Medium.ttf \
 *     -r '0x20-0x7F,0xA0-0xFF,0x2013,0x2014,0x2022,0x2026,0x2190-0x2194' \
 *     --font $S/'FontAwesome5-Solid+Brands+Regular.woff' -r '<Symbolliste>' \
 *     --format lvgl --lv-include lvgl.h --force-fast-kern-format \
 *     -o main/fonts/font_ui_14.c
 * @endcode
 */

#pragma once

#include "lvgl.h"

/*
 * Eine eigene Schrift mit genau einem Zeichen: der Sonne aus FontAwesome
 * (U+F185). Die Solarleistung wird daraus gesetzt — zehn Sonnen statt zehn
 * Rechtecken, und damit braucht der Balken keine Überschrift mehr.
 *
 * Erzeugt mit:
 *   lv_font_conv --no-compress --no-prefilter --bpp 4 --size 20 \
 *       --font $S/'FontAwesome5-Solid+Brands+Regular.woff' -r '0xF185' \
 *       --format lvgl --lv-include lvgl.h --force-fast-kern-format \
 *       -o main/fonts/font_sun_20.c
 */
LV_FONT_DECLARE(font_sun_20)

/** Die Sonne als UTF-8 — U+F185. */
#define SYMBOL_SUN "\xEF\x86\x85"

/*
 * Abwasser-Symbol: die Dusche aus FontAwesome (U+F2CC), 28 px. Sie steht für
 * das, was hineinläuft — Spüle und Dusche speisen den Grauwassertank.
 * Erzeugt wie font_sun_20, nur mit -r '0xF2CC' und --size 28.
 */
LV_FONT_DECLARE(font_waste_28)

/** Das Abwasserzeichen als UTF-8 — U+F2CC. */
#define SYMBOL_WASTE "\xEF\x8B\x8C"

LV_FONT_DECLARE(font_ui_14)
LV_FONT_DECLARE(font_ui_20)
LV_FONT_DECLARE(font_ui_28)
