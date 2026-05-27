#!/bin/sh
# Activates the ESP-IDF version declared in main/idf_component.yml.
# Source this script from the project root:
#
#   . tools/activate_idf.sh [carrot|spinach]
#
# The optional platform argument sets IDF_TARGET (carrot=esp32c6, spinach=esp32s3).
# When switching platforms, also run `idf.py set-target <target>` to regenerate
# sdkconfig — the build will error if sdkconfig was generated for a different target.
#
# When upgrading IDF, update the version in main/idf_component.yml (and
# components/kernel/idf_component.yml and .github/workflows/build.yml);
# this script will pick it up automatically.
VERSION=$(grep 'version:' main/idf_component.yml | tr -d ' "' | cut -d: -f2)
. "$HOME/.espressif/tools/activate_idf_v${VERSION}.sh"

case "$1" in
  carrot)  export IDF_TARGET=esp32c6 ;;
  spinach) export IDF_TARGET=esp32s3 ;;
  '')      ;;
  *)       echo "activate_idf.sh: unknown platform '$1' (use 'carrot' or 'spinach')" ;;
esac

# Ensure idf.py and the IDF Python environment are on PATH.
# The eim-based activation above may not update PATH in non-interactive shells
# (e.g. `bash -c '...'` subshells used by Claude Code).
export IDF_PATH="${HOME}/.espressif/v${VERSION}/esp-idf"
export IDF_PYTHON_ENV_PATH="${HOME}/.espressif/tools/python/v${VERSION}/venv"
case ":${PATH}:" in
    *":${IDF_PATH}/tools:"*) ;;
    *) export PATH="${IDF_PATH}/tools:${PATH}" ;;
esac
