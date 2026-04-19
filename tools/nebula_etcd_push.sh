#!/usr/bin/env bash
# nebula_etcd_push.sh - Discover Nebula devices and publish them to ETCD
#                       under /nebula/devices/<failure-domain>/<uuid>.
#
# Usage:
#   nebula_etcd_push.sh [--discover-bin PATH] [--etcdctl PATH]
#                       [--domain D] <path>...
#
# All arguments after the script's own flags are forwarded to nebula_discover.
# Requires: jq, etcdctl (unless --dry-run).
#
# Environment overrides:
#   NEBULA_DISCOVER       path to nebula_discover binary
#   ETCDCTL               path to etcdctl (default: etcdctl)
#   ETCDCTL_ENDPOINTS     passed through to etcdctl (comma-separated URLs)
#   NEBULA_FAILURE_DOMAIN used by nebula_discover if --domain not given
#
# Examples:
#   # Publish every Nebula device under /mnt/d/nebula to local etcd
#   tools/nebula_etcd_push.sh --domain rack-42 /mnt/d/nebula/
#
#   # Dry-run: just print the etcdctl commands
#   tools/nebula_etcd_push.sh --dry-run --domain rack-42 /mnt/d/nebula/
set -euo pipefail

DISCOVER_BIN="${NEBULA_DISCOVER:-$HOME/nebula-build/nebula_discover}"
ETCDCTL_BIN="${ETCDCTL:-etcdctl}"
DRY_RUN=0
PASSTHRU=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --discover-bin) DISCOVER_BIN="$2"; shift 2 ;;
    --etcdctl)      ETCDCTL_BIN="$2";  shift 2 ;;
    --dry-run)      DRY_RUN=1;          shift   ;;
    -h|--help)
      sed -n '2,22p' "$0"; exit 0 ;;
    *) PASSTHRU+=("$1"); shift ;;
  esac
done

if [[ ${#PASSTHRU[@]} -eq 0 ]]; then
  echo "error: no paths provided" >&2
  exit 2
fi
if [[ ! -x "$DISCOVER_BIN" ]]; then
  echo "error: nebula_discover not found/executable at: $DISCOVER_BIN" >&2
  exit 2
fi
if ! command -v jq >/dev/null; then
  echo "error: this script requires 'jq'" >&2
  exit 2
fi
if [[ $DRY_RUN -eq 0 ]] && ! command -v "$ETCDCTL_BIN" >/dev/null; then
  echo "error: etcdctl not found. Install it or re-run with --dry-run." >&2
  exit 2
fi

# Invoke discover, then per-device emit a `put <key> <json-value>`.
JSON_OUT="$("$DISCOVER_BIN" --format json "${PASSTHRU[@]}")"

echo "$JSON_OUT" | jq -c '.[]' | while read -r dev; do
  key="$(echo "$dev" | jq -r '.etcd_key')"
  val="$(echo "$dev" | jq -c '.')"    # store entire record as value
  if [[ $DRY_RUN -eq 1 ]]; then
    printf '%s put %q %q\n' "$ETCDCTL_BIN" "$key" "$val"
  else
    printf '%s -> ' "$key"
    "$ETCDCTL_BIN" put "$key" "$val"
  fi
done
