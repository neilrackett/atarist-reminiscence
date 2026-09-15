#!/bin/bash
# Check - and optionally fix - names destined for a GEMDOS volume.
# Copyright (C) 2026 Neil Rackett
#
# GEMDOS is 8.3 and uppercase: at most eight characters, an optional
# dot, at most three more. Anything longer is truncated by the
# filesystem rather than rejected, which is the problem - two files
# can collide silently, and the game then loads the wrong one or
# fails to find a file that is visibly present.
#
# extract-data.sh already names its output correctly. This is for
# data extracted with something else, which the README suggests for
# anyone without CAPS/SPS images.
set -eu

usage() {
    cat <<'EOF'
Usage: tools/check-names.sh [--fix] [dir]      (default dir: dist/)

  --fix   rename what can be renamed safely: case, an over-long name,
          an over-long extension.

Three things are reported rather than fixed, because each needs a
person:

  - a fixed name that is already taken. Two files wanting one name is
    the case that loses data, and picking a winner is not a script's
    decision.
  - odd characters. There is no right substitute, and inventing one
    risks colliding with a name that was already correct.
  - more than one dot. FILE.NAME.TXT could be FILENAME.TXT or
    FILE.NAM; only the person who made it knows which.

Exits non-zero if anything is still wrong afterwards, so it can gate
a release.
EOF
}

HERE="$(cd "$(dirname "$0")" && pwd)"
FIX=0
DIR=""
for arg in "$@"; do
    case "$arg" in
        --fix)     FIX=1 ;;
        -h|--help) usage; exit 0 ;;
        -*)        echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
        *)         DIR="$arg" ;;
    esac
done
DIR="${DIR:-$HERE/../dist}"

if [ ! -d "$DIR" ]; then
    echo "no such directory: $DIR" >&2
    exit 2
fi

# Split a name once. Callers read stem/ext/ndots rather than each
# working it out again - one definition, and no subshells: this runs
# per file and a fork costs more than every test here put together.
split_name() {
    name_stem="${1%%.*}"
    name_ext=""
    case "$1" in *.*) name_ext="${1##*.}" ;; esac
    ndots="${1//[!.]/}"
    ndots="${#ndots}"
}

# What GEMDOS would store this name as. Always answers - deciding
# whether renaming to it is SAFE is a separate question, asked below.
gemdos_name() {
    local s="${name_stem:0:8}" e="${name_ext:0:3}"
    # tr only when there is something to fold; most names are already
    # uppercase, and this is the only fork left in the common case.
    case "$s$e" in
        *[[:lower:]]*)
            s="$(printf '%s' "$s" | LC_ALL=C tr '[:lower:]' '[:upper:]')"
            e="$(printf '%s' "$e" | LC_ALL=C tr '[:lower:]' '[:upper:]')" ;;
    esac
    if [ -n "$e" ]; then gemdos="$s.$e"; else gemdos="$s"; fi
}

# Can this name be renamed without a human deciding something?
fixable() {
    [ "$ndots" -le 1 ] || return 1
    [ -n "$name_stem" ] || return 1
    case "$name_stem$name_ext" in *[!A-Za-z0-9_-]*) return 1 ;; esac
    return 0
}

if [ "$FIX" -eq 1 ]; then
    renamed=0
    blocked=0
    while IFS= read -r -d '' path; do
        name="${path##*/}"
        dir="${path%/*}"
        case "$name" in .*) continue ;; esac
        split_name "$name"
        fixable || continue
        gemdos_name
        [ "$gemdos" = "$name" ] && continue
        # -ef as well as -e: on a case-insensitive host (macOS,
        # Windows) "lower.spm" and "LOWER.SPM" are the same file, and
        # testing existence alone would block every case-only rename.
        if [ -e "$dir/$gemdos" ] && ! [ "$dir/$gemdos" -ef "$path" ]; then
            printf '  %-30s -> %-14s BLOCKED, that name is taken\n' "$name" "$gemdos"
            blocked=$((blocked + 1))
            continue
        fi
        # Two steps, because the host filesystem may be case-insensitive
        # and a case-only rename would otherwise be a no-op or an error.
        mv "$path" "$dir/.rename.$$" && mv "$dir/.rename.$$" "$dir/$gemdos"
        printf '  %-30s -> %s\n' "$name" "$gemdos"
        renamed=$((renamed + 1))
    done < <(find "$DIR" -type f -print0 | sort -z)
    echo
    echo "renamed $renamed file(s), $blocked blocked"
    echo
fi

bad=0
checked=0
# Collision keys carry their directory, so files are compared only
# against their own siblings without needing the walk to visit a
# directory contiguously - which it does not, since a subdirectory can
# sort between two of its parent's files.
seen=""

report() {
    printf '  %-30s %s\n' "$1" "$2"
    bad=$((bad + 1))
}

while IFS= read -r -d '' path; do
    name="${path##*/}"
    dir="${path%/*}"
    checked=$((checked + 1))
    case "$name" in .*) continue ;; esac      # host litter, not ours

    split_name "$name"

    if [ "$ndots" -gt 1 ]; then
        report "$name" "more than one dot (fix by hand)"
        continue
    fi

    if [ -z "$name_stem" ]; then
        report "$name" "no name before the dot"
    elif [ "${#name_stem}" -gt 8 ]; then
        report "$name" "name is ${#name_stem} characters, GEMDOS allows 8"
    fi

    if [ "${#name_ext}" -gt 3 ]; then
        report "$name" "extension is ${#name_ext} characters, GEMDOS allows 3"
    fi

    case "$name" in
        *[[:lower:]]*) report "$name" "not uppercase" ;;
    esac

    case "$name_stem$name_ext" in
        *[!A-Za-z0-9_-]*)
            report "$name" "has characters outside A-Z 0-9 _ - (fix by hand)" ;;
    esac

    gemdos_name
    case " $seen " in
        *" $dir/$gemdos "*)
            report "$name" "collides with another file as $gemdos" ;;
    esac
    seen="$seen $dir/$gemdos"
done < <(find "$DIR" -type f -print0 | sort -z)

echo
if [ "$bad" -eq 0 ]; then
    echo "$checked files in $DIR, all GEMDOS 8.3 clean"
else
    echo "$checked files in $DIR, $bad problem(s) above"
    [ "$FIX" -eq 0 ] && echo "Run with --fix to rename what can be renamed safely."
    exit 1
fi
