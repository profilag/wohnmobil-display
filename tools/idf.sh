#!/usr/bin/env bash
#
# Wrapper um idf.py, der die ESP-IDF-Umgebung selbst herstellt.
#
# Hintergrund: das System-python3 ist inzwischen 3.14, ESP-IDF 5.5.1 hat aber nur
# ein venv für 3.12. Ohne IDF_PYTHON_ENV_PATH bricht export.sh mit
# "virtual environment ... not found" ab. Diese Variable behebt das.
#
# Benutzung:  ./tools/idf.sh build
#             ./tools/idf.sh -p /dev/cu.usbmodem101 flash monitor
#
set -euo pipefail

IDF_ROOT="${IDF_ROOT:-$HOME/esp/esp-idf}"
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/python_env/idf5.5_py3.12_env}"

if [ ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
    echo "FEHLER: Python-venv nicht gefunden: $IDF_PYTHON_ENV_PATH" >&2
    echo "Vorhandene venvs:" >&2
    ls -1 "$HOME/.espressif/python_env" >&2 || true
    exit 1
fi

# shellcheck disable=SC1091
source "$IDF_ROOT/export.sh" > /dev/null

cd "$(dirname "$0")/.."
exec idf.py "$@"
