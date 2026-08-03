#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
profile=${ANYKEEP_DEB_PROFILE:-qt6}

case "${1-}" in
  -h|--help)
    echo "Usage: $0 [qt6|qt6-noble] [gbp buildpackage arguments...]"
    exit 0
    ;;
  qt6|qt6-noble)
    profile=$1
    shift
    ;;
esac

case "$profile" in
  qt6|qt6-noble)
    ;;
  *)
    echo "Usage: $0 [qt6|qt6-noble] [gbp buildpackage arguments...]" >&2
    exit 2
    ;;
esac

cd "$project_dir"

sudo apt-get install -y git-buildpackage
"$project_dir/packaging/build-deb" "$profile" -- sudo apt-get build-dep -y ./

"$script_dir/debrelease"

export QT_SELECT=6
exec "$project_dir/packaging/build-deb" "$profile" -- \
  gbp buildpackage --git-ignore-branch --git-ignore-new -us -uc -j10 "$@"
