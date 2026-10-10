#!/usr/bin/env bash
# 在指定目录跑完整流水线（烘焙 + 装配 + 校验），供本地验证用。
# 用法：bash tools/release/run_local_pipeline.sh [仓库根]
if [ -z "${BASH_VERSION:-}" ]; then
  echo "!! 本脚本需要 bash（不要用 sh 调用）" >&2; exit 1
fi
set -euo pipefail
ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$ROOT"
echo "仓库根: $ROOT"

: > /tmp/hta_pipeline.log

# ── 与 CI 对齐：把 texconv 的运行库复制到它旁边 ────────────────────────
#   仅在用 wine 跑 texconv（设了 HTA_TEXCONV）时才需要 —— Windows 本机跑
#   texconv 时这些 DLL 由系统提供。CI 上有同样的一步。
if [ -n "${HTA_TEXCONV:-}" ]; then
  for f in msvcp140.dll vcruntime140.dll vcruntime140_1.dll vcomp140.dll; do
    [ -f "dist/runtime-x64/$f" ] || { echo "!! 缺 dist/runtime-x64/$f" >&2; exit 1; }
    cp -f "dist/runtime-x64/$f" fontgen/
  done
  echo "已为 wine 复制 texconv 运行库（4 个）"
fi

bake() {
  local key="$1" fonts="$2" text="$3"
  echo "==> 烘焙 $key"
  python fontgen/build_cjk.py --src-fonts "$fonts" --text-dir "$text" \
    --out "baked_fonts/$key" >>/tmp/hta_pipeline.log 2>&1
}

rm -rf baked_fonts release
bake base Original_DATA/data/if/fonts/fonts.xml Original_DATA_CHS
bake dlc1 DLC1_DATA/data/if/fonts/fonts.xml     DLC1_DATA_CHS
bake dlc2 DLC2_DATA/data/if/fonts/fonts.xml     DLC2_DATA_CHS

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
