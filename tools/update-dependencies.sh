#!/bin/sh
# Re-resolves managed component versions and rewrites every committed lock file:
#
#   dependencies.<platform>.lock                     main firmware (shared by test/e2e-tests)
#   test/embedded-tests/dependencies.<platform>.lock embedded tests (adds catch2)
#
# Run it after changing any idf_component.yml, or to pick up newer versions within the manifests'
# constraints, then commit all lock files it changed. Resolving from scratch means every lock
# gets the newest matching versions, keeping the projects aligned with each other.
#
# Must be sourced, not executed, from the project root -- same as tools/build.sh, and for the same
# reason (see there):
#
#   . tools/update-dependencies.sh

_ud_update_status=0
for _ud_update_platform in carrot spinach; do
    . tools/activate_idf.sh "$_ud_update_platform"
    rm -f "dependencies.${_ud_update_platform}.lock" "test/embedded-tests/dependencies.${_ud_update_platform}.lock"
    idf.py -B "build-${_ud_update_platform}" reconfigure \
        && idf.py -C test/embedded-tests -B "test/embedded-tests/build-${_ud_update_platform}" reconfigure \
        || _ud_update_status=1
done
unset _ud_update_platform

git status --short -- 'dependencies.*.lock' 'test/embedded-tests/dependencies.*.lock'

return "$_ud_update_status" 2>/dev/null || exit "$_ud_update_status"
