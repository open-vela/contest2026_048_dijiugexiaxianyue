#!/usr/bin/env bash
#
# Rebuild the SF32LB52 Chuzhou map using the established source dataset and
# the established local 5 km grid layout.
#
# Names default to simplified Chinese. Traditional is --zh hant.
#
# Usage:
#   ./make_chuzhou_map.sh
#   ./make_chuzhou_map.sh --install
#   ./make_chuzhou_map.sh --zh hant
#
set -euo pipefail

OSM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
OPENVELA="$(cd "${OSM_DIR}/../../../.." && pwd -P)"
DATA_ROOT="${VMAP_CHUZHOU_DATA:-/home/jinsc/SDK/vela/osm_data/chuzhou}"
OSM_SOURCE="${DATA_ROOT}/chuzhou.osm"
VMAP_SOURCE="${DATA_ROOT}/vmap"
GRAPH_OUT="${DATA_ROOT}/graph.vgrf"
MAP_OUT="${VMAP_CHUZHOU_OUT:-/home/jinsc/SDK/vela/osm_data/map_chuzhou}"
PY="${OSM_DIR}/.venv/bin/python"
PACK_MAP="${OPENVELA}/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/tools/pack_map.py"
MKFS_MAP="${OPENVELA}/vendor/my_vendor/boards/sf32lb52/my_vendor/mkfs/fat/map"
INSTALL=no
ZH=hans

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --install) INSTALL=yes ;;
    --zh)
      shift
      ZH="${1:-}"
      if [[ "${ZH}" != "hans" && "${ZH}" != "hant" ]]; then
        echo "usage: $0 [--install] [--zh hans|hant]" >&2
        exit 2
      fi
      ;;
    *)
      echo "usage: $0 [--install] [--zh hans|hant]" >&2
      exit 2
      ;;
  esac
  shift
done

if [[ ! -x "${PY}" ]]; then
  echo "error: missing ${PY}; run python3 -m venv .venv and install requirements.txt" >&2
  exit 1
fi
if [[ ! -f "${OSM_SOURCE}" ]]; then
  echo "error: expected ${OSM_SOURCE}" >&2
  exit 1
fi

TMP_GRAPH="${GRAPH_OUT}.tmp.$$"
TMP_VMAP="${VMAP_SOURCE}.tmp.$$"
TMP_MAP="${MAP_OUT}.tmp.$$"
BACKUP="${MAP_OUT}.bak"
cleanup() {
  rm -f "${TMP_GRAPH}"
  rm -rf "${TMP_VMAP}" "${TMP_MAP}"
}
trap cleanup EXIT

echo "[vmap] ${OSM_SOURCE} -> ${VMAP_SOURCE}  (--zh ${ZH})"
# Established Chuzhou urban bbox (MAP_RECORD.md): S,W,N,E = 32.19,118.18,32.42,118.45
"${PY}" "${OSM_DIR}/build_vmap.py" \
  --osm "${OSM_SOURCE}" \
  --bbox 118.18,32.19,118.45,32.42 \
  --zoom 14 \
  --zh "${ZH}" \
  --out "${TMP_VMAP}"

echo "[graph] ${OSM_SOURCE} -> ${GRAPH_OUT}  (--zh ${ZH})"
"${PY}" "${OSM_DIR}/build_vgraph.py" \
  --osm "${OSM_SOURCE}" \
  --zh "${ZH}" \
  --out "${TMP_GRAPH}"

echo "[pack] local 5 km grid -> ${MAP_OUT}"
python3 "${PACK_MAP}" \
  -i "${TMP_VMAP}" \
  --graph "${TMP_GRAPH}" \
  -o "${TMP_MAP}" \
  --region-km 5 \
  --subdir \
  --max-zoom 14 \
  --clean

if [[ -e "${MAP_OUT}" ]]; then
  if [[ ! -e "${BACKUP}" ]]; then
    mv "${MAP_OUT}" "${BACKUP}"
  else
    rm -rf "${MAP_OUT}"
  fi
fi
rm -rf "${VMAP_SOURCE}"
mv "${TMP_VMAP}" "${VMAP_SOURCE}"
mv "${TMP_MAP}" "${MAP_OUT}"
mv "${TMP_GRAPH}" "${GRAPH_OUT}"

if [[ "${INSTALL}" == "yes" ]]; then
  echo "[install] ${MAP_OUT} -> ${MKFS_MAP}"
  rm -rf "${MKFS_MAP}"
  cp -a "${MAP_OUT}" "${MKFS_MAP}"
fi

echo "[done] ${MAP_OUT}  zh=${ZH}"
if [[ "${INSTALL}" != "yes" ]]; then
  echo "[hint] add --install to copy the result into mkfs/fat/map"
fi
