#!/bin/bash
# ============================================================================
# make_sd_img.sh <SD 卡容量>   —— 一条命令：编出固件 + 打包成 SD 整盘 .img
#
# 例：
#   ./make_sd_img.sh 2G                 # 2 GiB 卡
#   ./make_sd_img.sh 16G -o /tmp/a.img  # 16 GiB 卡，指定输出
#
# 本脚本**不依赖 build_board.py / vela_my_vendor_tools.py**（那支全量工具不在
# 开源范围内），只用底层小脚本：
#   nuttx/tools/build.sh                  （openvela 自带）编译 main / factory 两个配置
#   scripts/wrap_nuttx_image.sh           nuttx.bin -> nuttx.flash.bin（+CRC）
#   scripts/build_fs_root.sh              kv/lfs/fat 镜像（FAT 尺寸按 SD_PACK_CARD_MIB）
#   scripts/pack_sd_img.py                打进整盘 .img（逻辑大小 = 卡容量）
#
# 只产出 .img，不烧录、不监视。
# ============================================================================
set -euo pipefail

SIZE="${1:-}"
shift || true

OUT=""
JOBS=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out) OUT="${2:-}"; shift 2 ;;
    -j|--jobs) JOBS="${2:-}"; shift 2 ;;
    *) echo "未知参数: $1（可用: -o <输出路径> -j <并行数>）" >&2; exit 2 ;;
  esac
done

if [ -z "$SIZE" ]; then
  echo "用法: $0 <SD 卡容量，如 2G / 16G / 32768M> [-o 输出.img] [-j N]" >&2
  exit 2
fi

case "$SIZE" in
  *[Gg]) MIB=$(( ${SIZE%[Gg]} * 1024 )) ;;
  *[Mm]) MIB=${SIZE%[Mm]} ;;
  *[0-9]) MIB="$SIZE" ;;
  *) echo "容量写法: 2G 或 32768M（也可直接给 MiB 数字）" >&2; exit 2 ;;
esac
if [ "$MIB" -lt 2048 ]; then
  echo "卡容量太小（${MIB} MiB）：布局里 FAT 区从 1 GiB 起，最小支持 2048 MiB（2G 卡）" >&2
  exit 2
fi
[ -n "$JOBS" ] || JOBS="$(nproc 2>/dev/null || echo 8)"

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TREE="$(cd "$HERE" && pwd)"
ROOT="$(cd "$TREE/../.." && pwd)"
[ -x "$ROOT/build.sh" ] || { echo "找不到 $ROOT/build.sh（脚本要放在 vendor/<树>/ 下）" >&2; exit 1; }
[ -f "$TREE/scripts/build_fs_root.sh" ] || { echo "本树不是完整 vendor 树，缺少 scripts/" >&2; exit 1; }
cd "$ROOT"

BOARD="$TREE/boards/sf32lb52/helmone"
MAIN_CFG="$BOARD/configs/nsh/"
FACT_CFG="$BOARD/configs/nsh-factory/"
MAIN_OUT="$ROOT/cmake_out/helmone_nsh"
FACT_OUT="$ROOT/cmake_out/helmone_nsh-factory"

echo "=== 1/4 main 槽固件（$MAIN_CFG）==="
./build.sh "$MAIN_CFG" --cmake -j"$JOBS"
bash "$TREE/scripts/wrap_nuttx_image.sh" "$MAIN_OUT"

echo "=== 2/4 factory 槽固件（$FACT_CFG；pack-sd-img 需要它）==="
./build.sh "$FACT_CFG" --cmake -j"$JOBS"
bash "$TREE/scripts/wrap_nuttx_image.sh" "$FACT_OUT"

if [ -d "$TREE/boot_loader/project" ]; then
  echo "=== 3/4 二级 boot：本树含 project，重建 ftab.bin + bootloader.bin ==="
  bash "$TREE/boot_loader/build.sh" 2>/dev/null || echo "  （二级 boot 重建失败，改用随仓 bin）"
else
  echo "=== 3/4 二级 boot：用随仓的 boot_loader/bin 预编译件（本树未含 project）==="
fi

# **按目标卡容量重建 FAT + 分区表**：ptab 里 regions[10]（FAT 区，max_size=0 到卡尾）
# 的 custom.SD_PACK_CARD_MIB 默认 14832（≈16G 卡）。卡与布局不匹配时（ptab 原话）
# 设备会 EINVAL 挂不上，所以这里把它临时改成目标值，跑完还原（traps 收尾）。
PTAB="$TREE/boot_loader/config/nsh/ptab.sdmmc.json"
if [ -f "$PTAB" ]; then
  PTAB_OLD="$(python3 -c "
import json,sys
d=json.load(open('$PTAB'))
print(d[0]['regions'][10]['custom']['SD_PACK_CARD_MIB'])
" 2>/dev/null || echo "")"
  patch_ptab() {
    python3 - "$PTAB" "$1" <<'PY'
import json, sys
path, mib = sys.argv[1], int(sys.argv[2])
with open(path, "r", encoding="utf-8") as fp:
    d = json.load(fp)
old = d[0]["regions"][10]["custom"].get("SD_PACK_CARD_MIB")
d[0]["regions"][10]["custom"]["SD_PACK_CARD_MIB"] = mib
with open(path, "w", encoding="utf-8") as fp:
    json.dump(d, fp, ensure_ascii=False, indent=2)
    fp.write("\n")
print(f"  ptab SD_PACK_CARD_MIB: {old} -> {mib}")
PY
  }
  restore_ptab() {
    if [ -n "$PTAB_OLD" ]; then
      patch_ptab "$PTAB_OLD" >/dev/null
      echo "  ptab 已还原为 $PTAB_OLD"
    fi
  }
  trap restore_ptab EXIT
  echo "=== 分区表按 ${MIB} MiB 卡调整 ==="
  patch_ptab "$MIB"
else
  echo "警告: 找不到 $PTAB，分区表保持原样" >&2
fi

echo "=== 4/4 FS + 打包（FAT 与镜像都按 ${MIB} MiB 卡）==="
SD_PACK_CARD_MIB="$MIB" bash "$TREE/scripts/build_fs_root.sh"

if [ "$((MIB % 1024))" -eq 0 ]; then
  SUFFIX="$((MIB / 1024))g"
else
  SUFFIX="${MIB}m"
fi
IMG="${OUT:-$TREE/boot_loader/bin/helmone_sd_${SUFFIX}.img}"
python3 "$TREE/scripts/pack_sd_img.py" --card-mib "$MIB" --full-size -o "$IMG"

ls -lh "$IMG"
echo
echo "完成：$IMG   （逻辑大小 ${MIB} MiB）"
echo "写入整盘 SD（慎用，会覆盖目标盘）："
echo "  sudo dd if='$IMG' of=/dev/sdX bs=4M status=progress conv=fsync"
