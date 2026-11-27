#!/bin/sh
set -eu
bundle_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
exec "$bundle_dir/MikanEngine" --project "$bundle_dir/projects/game" --no-editor "$@"
