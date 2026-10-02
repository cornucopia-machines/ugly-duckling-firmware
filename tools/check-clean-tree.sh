#!/bin/sh
# Fails if the working tree has any uncommitted changes or untracked files.
#
# Run in CI right after a build: the component manager rewrites a dependencies.<platform>.lock that
# no longer matches the manifests, so a lock that wasn't refreshed and committed along with an
# idf_component.yml change shows up here -- as does anything else a build unexpectedly regenerates.

status=$(git status --porcelain)
if [ -z "$status" ]; then
    exit 0
fi

echo "::error::The build modified or created files in the source tree. If a dependencies.*.lock" \
    "changed, run '. tools/update-dependencies.sh' locally and commit the lock files."
echo "$status"
git --no-pager diff
exit 1
