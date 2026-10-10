/**
 * @file secrets_select.h
 * @brief Bindet secrets.h ein, ersatzweise die Vorlage mit Platzhaltern.
 */
#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "main/secrets.h fehlt — es gelten die Platzhalter aus secrets.example.h"
#include "secrets.example.h"
#endif
