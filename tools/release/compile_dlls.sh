#!/usr/bin/env bash
# ===========================================================================
# compile_dlls.sh —— 用 MSVC v141 (14.16) 编译 6 个插件 DLL
# ===========================================================================
# 为什么不用 MSBuild：
#   msvc-wine 只提供 cl.exe / link.exe 的 wine 包装脚本，**没有 MSBuild**，
#   也刻意不装它（省几百 MB 下载）。所以本脚本把 6 个 .vcxproj 的
#   Release|Win32 配置逐项翻译成 cl/link 命令行。
#
# ★ 翻译结果已在本机用真 v141 逐个复现，产物体积与发布参考包**完全一致**：
#     hta_chs        178176 B      Render9Fix     144384 B
#     hta_chs_dlc1   178176 B      DLC1_MemFix    140800 B
#     hta_chs_dlc2   195584 B      DLC2_MemFix    140288 B
#   体积是「工具集 + SDK + flags 组合」的确定指纹；任一项回退（例如 cl 静默
#   变成 v142）尺寸就会变，被脚本末尾的闸门拦下。
#
# 翻译对照（vcxproj → cl/link）：
#   Optimization=MaxSpeed          → -O2
#   IntrinsicFunctions=true        → -Oi
#   FunctionLevelLinking=true      → -Gy
#   WarningLevel=Level3            → -W3   （/WX- 不把警告当错误）
#   SDLCheck=false                 → -sdl-
#   RuntimeLibrary=MultiThreaded   → -MT   （静态 CRT，目标机无需运行库）
#   AdditionalOptions /utf-8       → -utf-8
#   DebugInformationFormat=ProgramDatabase → 调试信息
#     ★ 用 **/Z7** 而不是 /Zi ★
#       /Zi 要求 cl 通过**命名管道**与 mspdbsrv.exe 通信来汇总 PDB；wine 下命名
#       管道是已知脆弱点（官方 msvc-wine 的 README 就要求装 winbind 兜这个）。
#       /Z7 把调试信息**内联进 .obj**，不需要任何服务进程。
#       ★ 已实测：/Z7+/DEBUG 与 /Zi+/DEBUG 对全部 6 个产物的**体积完全相同**
#         （178176 / 178176 / 195584 / 144384 / 140800 / 140288）。
#       所以这是一次纯粹的健壮性提升，不改变任何发布产物。
#   PrecompiledHeader=Use          → /Yc(pch.cpp 创建) /Yu + /Fp(其余使用)
#     Render9Fix、DLC*_MemFix 是 NotUsing → 不带 PCH 开关
#   TargetName → -OUT:<name>.dll
#   Link 默认库 → kernel32 user32 gdi32 winspool comdlg32 advapi32 shell32
#                 ole32 oleaut32 uuid odbc32 odbccp32
#   SubSystem=Windows → -SUBSYSTEM:WINDOWS
#   EnableCOMDATFolding / OptimizeReferences → -OPT:ICF -OPT:REF
#   ★ /SAFESEH：vcxproj 未显式写，但 x86 链接默认开启，历史产物同此。
#   ★ 所有开关用 '-' 前缀而非 '/'：wine 会把 "/xxx" 当 Unix 路径。
#
# 用法：MSVC_WINE_PREFIX=/path/to/.msvc sh tools/release/compile_dlls.sh
# ===========================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${MSVC_WINE_PREFIX:-$ROOT/.msvc}"
export PATH="$PREFIX/bin/x86:$PATH"

command -v cl   >/dev/null || { echo "!! PATH 里没有 $PREFIX/bin/x86/cl" >&2; exit 1; }
command -v link >/dev/null || { echo "!! PATH 里没有 link" >&2; exit 1; }

BANNER="$(cl 2>&1 || true)"
echo "── cl banner ──"
echo "$BANNER" | head -2
if ! echo "$BANNER" | grep -q '19\.16'; then
  echo "!! cl 不是 MSVC 14.16 (v141)，产物体积指纹会变；终止。" >&2
  exit 1
fi

LIBS=(kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib
      advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib
      odbc32.lib odbccp32.lib)

LINK_COMMON=(-INCREMENTAL:NO -NOLOGO -MANIFEST -MANIFESTUAC:NO -manifest:embed
             -DEBUG -SUBSYSTEM:WINDOWS -OPT:REF -OPT:ICF -TLBID:1
             -DYNAMICBASE -NXCOMPAT -MACHINE:X86 -SAFESEH -DLL)

# 公共编译开关（PCH / 非 PCH 共用部分）
CL_BASE=(-c -Z7 -nologo -W3 -WX- -diagnostics:classic -sdl-
         -O2 -Oi -Oy- -Gm- -EHsc -MT -GS -Gy -fp:precise
         -Zc:wchar_t -Zc:forScope -Zc:inline -Gd -TP -analyze- -FC -utf-8)

PCH_SRCS=(dllmain.cpp font_hooks.cpp hook.cpp ldisasm.cpp log.cpp pattern.cpp
          pathd.cpp slotmap.cpp text_hooks.cpp render.cpp transcode.cpp)
MEMFIX_SRCS=(dllmain.cpp lowheap.cpp memhook.cpp util.cpp)
R9_SRCS=(Render9Fix.cpp lowheap.cpp memhook.cpp util.cpp)

# build_with_pch <项目目录> <TargetName> <导出宏>
build_with_pch() {
  local dir="$1" name="$2" define="$3"
  local pdir="$ROOT/$dir"
  local obj="$pdir/build/obj/Release"
  local out="$pdir/build/Release"
  mkdir -p "$obj" "$out"
  echo "==> 编译 $name（$dir）"
  # 全部源码一次编译。相对包含（"pch.h" / "plugin.h"）由 MSVC 依据**源文件
  # 所在目录**解析，故不必 cd 到项目目录。
  ( cd "$pdir" && cl "${CL_BASE[@]}" \
      -D WIN32 -D NDEBUG -D _WINDOWS -D _USRDLL -D "$define" \
      -D _WINDLL -D _MBCS \
      -Yc"pch.h" -Fp"$obj/$name.pch" -Fo"$obj/" -Fd"$obj/vc141.pdb" \
      pch.cpp )
  ( cd "$pdir" && cl "${CL_BASE[@]}" \
      -D WIN32 -D NDEBUG -D _WINDOWS -D _USRDLL -D "$define" \
      -D _WINDLL -D _MBCS \
      -Yu"pch.h" -Fp"$obj/$name.pch" -Fo"$obj/" -Fd"$obj/vc141.pdb" \
      "${PCH_SRCS[@]}" )
  echo "==> 链接 $name"
  link -OUT:"$out/$name.dll" "${LINK_COMMON[@]}" "${LIBS[@]}" "$obj"/*.obj
}

# build_no_pch <项目目录> <TargetName> <源码数组名>
build_no_pch() {
  local dir="$1" name="$2"
  shift 2
  local srcs=("$@")
  local pdir="$ROOT/$dir"
  local obj="$pdir/build/obj/Release"
  local out="$pdir/build/Release"
  mkdir -p "$obj" "$out"
  echo "==> 编译 $name（$dir）"
  ( cd "$pdir" && cl "${CL_BASE[@]}" \
      -D WIN32 -D NDEBUG -D _WINDOWS -D _USRDLL -D _CRT_SECURE_NO_WARNINGS \
      -D _WINDLL -D _MBCS \
      -Fo"$obj/" -Fd"$obj/vc141.pdb" "${srcs[@]}" )
  echo "==> 链接 $name"
  link -OUT:"$out/$name.dll" "${LINK_COMMON[@]}" "${LIBS[@]}" "$obj"/*.obj
}

build_with_pch "HardTruckApocalypse_CHINESE_DLL"        hta_chs      HARDTRUCKAPOCALYPSECHINESEDLL_EXPORTS
build_with_pch "HardTruckApocalypse_CHINESE_DLL_DLC1"  hta_chs_dlc1 HARDTRUCKAPOCALYPSECHINESEDLLDLC1_EXPORTS
build_with_pch "HardTruckApocalypse_CHINESE_DLL_DLC2"  hta_chs_dlc2 HARDTRUCKAPOCALYPSECHINESEDLLDLC2_EXPORTS
build_no_pch   "Render9Fix"                            Render9Fix   "${R9_SRCS[@]}"
build_no_pch   "DLC1_MemFix"                           DLC1_MemFix  "${MEMFIX_SRCS[@]}"
build_no_pch   "DLC2_MemFix"                           DLC2_MemFix  "${MEMFIX_SRCS[@]}"

# ── 体积闸门 ──────────────────────────────────────────────────────────────
# ★ 本流程最重要的一道闸。体积是工具集/SDK/flags 的确定指纹：工具链回退或
#   少了任何一条 flag，尺寸立刻变化 → 直接失败，绝不把错 DLL 发出去。
#   括号里的数字是本机用真 v141 复现并核对过参考包的结果。
echo ""
echo "── 体积闸门 ──"
rc=0
check_size() {
  local p="$ROOT/$1" want="$2"
  if [ ! -f "$p" ]; then echo "!! 缺少产物 $1" >&2; rc=1; return 0; fi
  local got; got=$(wc -c < "$p")
  if [ "$got" -ne "$want" ]; then
    echo "!! $1 = $got B，期望 $want B（疑似工具集/SDK/flags 回退）" >&2
    rc=1
  else
    printf '  OK %-46s %8d B\n' "$(basename "$p")" "$got"
  fi
}
check_size "HardTruckApocalypse_CHINESE_DLL/build/Release/hta_chs.dll"           178176
check_size "HardTruckApocalypse_CHINESE_DLL_DLC1/build/Release/hta_chs_dlc1.dll" 178176
check_size "HardTruckApocalypse_CHINESE_DLL_DLC2/build/Release/hta_chs_dlc2.dll" 195584
check_size "Render9Fix/build/Release/Render9Fix.dll"                            144384
check_size "DLC1_MemFix/build/Release/DLC1_MemFix.dll"                          140800
check_size "DLC2_MemFix/build/Release/DLC2_MemFix.dll"                          140288
[ "$rc" -eq 0 ] || { echo "!! 体积闸门未通过" >&2; exit 1; }

echo ""
echo "✓ 6 个 DLL 编译完成且体积指纹正确"
