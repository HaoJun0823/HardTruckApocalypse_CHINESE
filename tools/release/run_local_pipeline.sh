#!/usr/bin/env bash
# 在指定目录跑完整流水线（烘焙 + 装配 + 校验），供本地验证用。
# 用法：bash tools/release/run_local_pipeline.sh [仓库根]
set -euo pipefail
ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$ROOT"
echo "仓库根: $ROOT"

: > /tmp/hta_pipeline.log

bake() {
  local key="$1" fonts="$2" text="$3"
  echo "==> 烘焙 $key"
  python fontgen/build_cjk.py --src-fonts "$fonts" --text-dir "$text" \
    --out "baked_fonts/$key" >>/tmp/hta_pipeline.log 2>&1
}

rm -rf baked_fonts release
bake base Original_DATA/data/if/fonts/fonts.xml Original_DATA_CHS
bake dlc1 DLC1_DATA/data/if/fonts/fonts.xml     DLC1_DATA_CHS
bake dlc2 DLC2_DATA_CHS/data/if/fonts/fonts.xml DLC2_DATA_CHS

echo "==> 装配"
python tools/release/build_release.py --out release --fonts baked_fonts \
  --date "${HTA_DATE:-20261009}" >>/tmp/hta_pipeline.log 2>&1

echo "==> 校验"
python tools/release/verify_release.py release

echo "==> 与参考包比对（仅当参考包存在）"
if [ -d "/i/LocalGames/Hard Truck Apocalypse/Hard_Truck_Apocalypse_CHS_V0-FIX1" ]; then
  python tools/release/compare_with_ref.py release || true
else
  echo "   (本机无参考包，跳过)"
fi
echo "✓ 完成；详细日志 /tmp/hta_pipeline.log"
