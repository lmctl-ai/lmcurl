#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$ROOT/deps.lock"
DEST="$ROOT/third_party"
mkdir -p "$DEST"
sha256() { sha256sum "$1" | awk '{print $1}'; }
fetch_one() {
  local name="$1" url="$2" want="$3" archive="$4" dir="$5"
  local tarball="$DEST/.${archive}"
  if [[ ! -f "$tarball" || "$(sha256 "$tarball")" != "$want" ]]; then
    timeout 300 curl --fail --location --retry 3 --retry-delay 2 "$url" -o "$tarball"
  fi
  [[ "$(sha256 "$tarball")" == "$want" ]] || { echo "$name archive hash mismatch" >&2; exit 1; }
  if [[ -f "$DEST/$dir/.lmcurl-archive-sha256" && "$(cat "$DEST/$dir/.lmcurl-archive-sha256")" == "$want" ]]; then return; fi
  rm -rf "$DEST/$dir"
  mkdir -p "$DEST/$dir"
  tar -xf "$tarball" -C "$DEST/$dir" --strip-components=1
  printf '%s\n' "$want" > "$DEST/$dir/.lmcurl-archive-sha256"
}
get() { sed -n "s/^$1\.$2=//p" "$LOCK"; }
if [[ "${1:-}" == "--verify" ]]; then
  for spec in \
    'curl curl-8.23.0.tar.xz curl' \
    'openssl openssl-3.5.9.tar.gz openssl' \
    'nghttp2 nghttp2-1.70.0.tar.xz nghttp2' \
    'zlib zlib-1.3.1.tar.gz zlib' \
    'nlohmann_json json-3.11.3.tar.xz nlohmann' \
    'valijson valijson-0.7.tar.gz valijson' \
    'libxml2 libxml2-2.15.4.tar.xz libxml2'; do
    read -r name archive dir <<<"$spec"
    [[ -f "$DEST/$dir/.lmcurl-archive-sha256" ]] || { echo "$name missing; run scripts/sync-deps.sh" >&2; exit 1; }
    [[ -f "$DEST/.$archive" && "$(sha256 "$DEST/.$archive")" == "$(get "$name" sha256)" ]] || { echo "$name cached archive differs from deps.lock" >&2; exit 1; }
    [[ "$(cat "$DEST/$dir/.lmcurl-archive-sha256")" == "$(get "$name" sha256)" ]] || { echo "$name pin mismatch" >&2; exit 1; }
  done
  echo "lmcurl third-party archives match deps.lock"
  exit 0
fi
fetch_one curl "$(get curl url)" "$(get curl sha256)" curl-8.23.0.tar.xz curl
fetch_one openssl "$(get openssl url)" "$(get openssl sha256)" openssl-3.5.9.tar.gz openssl
fetch_one nghttp2 "$(get nghttp2 url)" "$(get nghttp2 sha256)" nghttp2-1.70.0.tar.xz nghttp2
fetch_one zlib "$(get zlib url)" "$(get zlib sha256)" zlib-1.3.1.tar.gz zlib
fetch_one nlohmann_json "$(get nlohmann_json url)" "$(get nlohmann_json sha256)" json-3.11.3.tar.xz nlohmann
fetch_one valijson "$(get valijson url)" "$(get valijson sha256)" valijson-0.7.tar.gz valijson
fetch_one libxml2 "$(get libxml2 url)" "$(get libxml2 sha256)" libxml2-2.15.4.tar.xz libxml2
echo "synced pinned lmcurl dependencies"
