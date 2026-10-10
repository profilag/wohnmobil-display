"""Read-only BLE client for HumsiENK LiFePO4 batteries.

Sicherheitsgrundsatz dieses Pakets:

    Es werden KEINE BMS-Konfigurationsparameter geschrieben.

Erlaubt sind ausschliesslich:
  * BLE-Scan (passiv)
  * GATT-Discovery
  * Lesen von Characteristics
  * Aktivieren von Notifications
  * dokumentierte Auth-/Read-Telegramme (ab Phase 5, separat freigegeben)
"""

from __future__ import annotations

__version__ = "0.1.0"
__all__ = ["__version__"]
