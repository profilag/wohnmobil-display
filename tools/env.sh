# In ein interaktives Terminal einlesen, um idf.py direkt benutzen zu können:
#
#     source tools/env.sh
#     idf.py build flash monitor
#
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/python_env/idf5.5_py3.12_env}"
source "${IDF_ROOT:-$HOME/esp/esp-idf}/export.sh"
