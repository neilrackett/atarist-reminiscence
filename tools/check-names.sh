#!/bin/bash
# Check - and optionally fix - names destined for a GEMDOS volume.
# Copyright (C) 2026 Neil Rackett
#
# Usage: tools/check-names.sh [--fix] [dir]      (default dir: dist/)
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
#
# --fix renames what can be renamed safely: case, an over-long name,
# an over-long extension. It deliberately will NOT touch:
#
#   - anything whose fixed name already exists. Two files wanting one
#     name is the case that loses data, and picking a winner is not a
#     script's decision to make.
#   - odd characters. There is no right substitute, and inventing one
#     risks colliding with a name that is already correct.
#   - more than one dot. FILE.NAME.TXT could be FILENAME.TXT or
#     FILE.NAM; only the person who made it knows which.
#
# Those are reported for you to sort out by hand. Exits non-zero if
# anything is still wrong afterwards, so it can gate a release.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

FIX=0
DIR=""
for arg in "$@"; do
    case "$arg" in
        --fix) FIX=1 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) DIR="$arg" ;;
    esac
done
DIR="${DIR:-$HERE/../dist}"

if [ ! -d "$DIR" ]; then
    echo "no such directory: $DIR" >&2
    exit 2
fi

# What GEMDOS would make of a name, where that is unambiguous.
# Prints nothing when the name cannot be fixed automatically.
target_name() {
    local name="$1" stem ext dots
    dots="${name//[!.]/}"
    [ "${#dots}" -gt 1 ] && return 0          # ambiguous, leave alone
    stem="${name%%.*}"
    ext=""
    case "$name" in *.*) ext="${name##*.}" ;; esac
    [ -z "$stem" ] && return 0
    printf '%s' "$stem$ext" | grep -q '[^A-Za-z0-9_-]' && return 0
    stem="$(printf '%s' "${stem:0:8}" | tr '[:lower:]' '[:upper:]')"
    ext="$(printf '%s' "${ext:0:3}" | tr '[:lower:]' '[:upper:]')"
    if [ -n "$ext" ]; then printf '%s.%s' "$stem" "$ext"; else printf '%s' "$stem"; fi
}

if [ "$FIX" -eq 1 ]; then
    renamed=0
    blocked=0
    while IFS= read -r -d '' path; do
        name="$(basename "$path")"
        dir="$(dirname "$path")"
        case "$name" in .*) continue ;; esac
        want="$(target_name "$name")"
        [ -z "$want" ] && continue            # not automatically fixable
        [ "$want" = "$name" ] && continue     # already right
        # -ef as well as -e: on a case-insensitive host (macOS,
        # Windows) "lower.spm" and "LOWER.SPM" are the same file, and
        # testing existence alone would block every case-only rename.
        if [ -e "$dir/$want" ] && ! [ "$dir/$want" -ef "$dir/$name" ]; then
            printf '  %-30s -> %-14s BLOCKED, that name is taken\n' "$name" "$want"
            blocked=$((blocked + 1))
            continue
        fi
        # Two steps, because the host filesystem may be case-insensitive
        # and a case-only rename would otherwise be a no-op or an error.
        mv "$dir/$name" "$dir/.rename.$$" && mv "$dir/.rename.$$" "$dir/$want"
        printf '  %-30s -> %s\n' "$name" "$want"
        renamed=$((renamed + 1))
    done < <(find "$DIR" -type f -print0 | sort -z)
    echo
    echo "renamed $renamed file(s)${blocked:+, $blocked blocked}"
    echo
fi

bad=0
checked=0
prev_dir=""
seen=""

report() {
    printf '  %-30s %s\n' "$1" "$2"
    bad=$((bad + 1))
}

while IFS= read -r -d '' path; do
    name="$(basename "$path")"
    dir="$(dirname "$path")"
    checked=$((checked + 1))

    if [ "$dir" != "$prev_dir" ]; then
        seen=""
        prev_dir="$dir"
    fi

    case "$name" in .*) continue ;; esac      # host litter, not ours

    stem="${name%%.*}"
    ext=""
    case "$name" in *.*) ext="${name##*.}" ;; esac

    dots="${name//[!.]/}"
    if [ "${#dots}" -gt 1 ]; then
        report "$name" "more than one dot (fix by hand)"
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

    if printf '%s' "$stem$ext" | grep -q '[^A-Za-z0-9_-]'; then
        report "$name" "has characters outside A-Z 0-9 _ - (fix by hand)"
    fi

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
    [ "$FIX" -eq 0 ] && echo "Run with --fix to rename what can be renamed safely."
    exit 1
fi
