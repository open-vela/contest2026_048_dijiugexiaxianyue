#!/usr/bin/env bash
# Preview a packed map/ in the browser (lon*/lat*/x*_y*.vpk, no map.idx).
#   ./view_map.sh
#   ./view_map.sh /home/jinsc/SDK/vela/osm_data/map
set -euo pipefail
WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
MAP_DIR="${1:-${VMAP_CHINA_MAP:-/home/jinsc/SDK/vela/osm_data/map}}"
exec python3 "${WEB_DIR}/view_map.py" --dir "${MAP_DIR}"
