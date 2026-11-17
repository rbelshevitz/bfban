#!/bin/sh
set -eu

# Safe by default: fetch and validate, but do not block anything.
url="https://feodotracker.abuse.ch/downloads/ipblocklist_recommended.txt"
output="/tmp/bfban-threat-feed.$$"
apply=0

for arg in "$@"; do
    case "$arg" in
        --apply) apply=1 ;;
        --url=*) url=${arg#*=} ;;
        --output=*) output=${arg#*=} ;;
        --help) echo "Usage: fetch-threat-feed.sh [--apply] [--url=URL] [--output=FILE]"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

command -v curl >/dev/null 2>&1 || { echo "curl is required" >&2; exit 1; }
trap 'rm -f "$output"' EXIT HUP INT TERM

curl --fail --location --silent --show-error --max-time 60 "$url" |
    awk '/^[[:space:]]*[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+[[:space:]]*$/ {gsub(/[[:space:]]/, "", $0); print}' |
    sort -u > "$output"

count=$(wc -l < "$output")
echo "validated $count IPv4 addresses from $url"
echo "feed saved temporarily as $output"

if [ "$apply" -eq 1 ]; then
    command -v bfban >/dev/null 2>&1 || { echo "bfban is required for --apply" >&2; exit 1; }
    while IFS= read -r ip; do
        [ -n "$ip" ] && bfban add "$ip"
    done < "$output"
    echo "applied $count addresses to bfban"
else
    echo "not applied; review the feed and rerun with --apply if appropriate"
fi
