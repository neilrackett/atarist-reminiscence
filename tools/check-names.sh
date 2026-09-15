#!/bin/bash
# Check that everything destined for the ST can live on a GEMDOS
# volume.
# Copyright (C) 2026 Neil Rackett
#
# Usage: tools/check-names.sh [dir]        (default: dist/)
#
# GEMDOS is 8.3 and uppercase: a name is at most eight characters,
# an optional dot, and at most three more. Anything longer is
# truncated by the filesystem rather than rejected, which is the
# problem - two files can collide silently and the game then loads
# the wrong one, or fails to find a file that is visibly present.
# extract-data.sh already names its output correctly; this exists
# for data extracted with something else, which the README suggests
# for anyone whose disks have gone.
#
# Exits non-zero if anything is wrong, so it can gate a release.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:-$HERE/../dist}"

if [ ! -d "$DIR" ]; then
    echo "no such directory: $DIR" >&2
    exit 2
fi

bad=0
checked=0

report() {
    printf '  %-30s %s\n' "$1" "$2"
    bad=$((bad + 1))
}

# Collisions are per directory, and are what actually breaks a game:
# REPLICANT.SPM and REPLICAN.SPM are different names to a host and
# the same eight characters to GEMDOS.
prev_dir=""
seen=""

while IFS= read -r -d '' path; do
    name="$(basename "$path")"
    dir="$(dirname "$path")"
    checked=$((checked + 1))

    if [ "$dir" != "$prev_dir" ]; then
        seen=""
        prev_dir="$dir"
    fi

    case "$name" in
        .*) continue ;;                 # .DS_Store and friends: host litter
    esac

    stem="${name%%.*}"
    ext=""
    case "$name" in
        *.*) ext="${name##*.}" ;;
    esac

    # More than one dot is not expressible at all
    dots="${name//[!.]/}"
    if [ "${#dots}" -gt 1 ]; then
        report "$name" "more than one dot"
        continue
    fi

    if [ "${#stem}" -eq 0 ]; then
        report "$name" "no name before the dot"
    elif [ "${#stem}" -gt 8 ]; then
        report "$name" "name is ${#stem} characters, GEMDOS allows 8"
    fi

    if [ "${#ext}" -gt 3 ]; then
        report "$name" "extension is ${#ext} characters, GEMDOS allows 3"
    fi

    if [ "$name" != "$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')" ]; then
        report "$name" "not uppercase"
    fi

    # Letters, digits, underscore and hyphen are safe everywhere. The
    # wider GEMDOS set is not worth the risk across TOS versions.
    if printf '%s' "$stem$ext" | grep -q '[^A-Za-z0-9_-]'; then
        report "$name" "has characters outside A-Z 0-9 _ -"
    fi

    # What GEMDOS would actually store, to catch silent collisions
    up="$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')"
    trunc="$(printf '%s' "${stem:0:8}" | tr '[:lower:]' '[:upper:]')"
    [ -n "$ext" ] && trunc="$trunc.$(printf '%s' "${ext:0:3}" | tr '[:lower:]' '[:upper:]')"
    case " $seen " in
        *" $trunc "*) report "$name" "collides with another file as $trunc" ;;
    esac
    seen="$seen $trunc"
done < <(find "$DIR" -type f -print0 | sort -z)

echo
if [ "$bad" -eq 0 ]; then
    echo "$checked files in $DIR, all GEMDOS 8.3 clean"
else
    echo "$checked files in $DIR, $bad problem(s) above"
    exit 1
fi
