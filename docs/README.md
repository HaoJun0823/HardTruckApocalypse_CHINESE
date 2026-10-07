# 文档索引

本项目（Hard Truck Apocalypse 中文汉化 + 老引擎兼容修复）的文档入口。
按"**结论 → 证据 → 复现**"组织，凡是推测都会明确标注，凡是实测都带日志/地址证据。

---

## 核心文档

| 文档 | 内容 | 状态 |
|---|---|---|
| [LAA 内存扩展崩溃：根因与 Render9Fix](2026-10-07-LAA内存扩展崩溃-根因与Render9Fix.md) | hta.exe 开 LAA 后进世界崩在 `dxrender9+0x416D6` 的完整取证链、排除的假设、静态验证（IDA）、修复与实机验证 | ✅ 已修复并验证 |
| [汉化冻结门：时序失败根因与修复](2026-10-07-汉化冻结门时序失败-根因与修复.md) | 失败对话框乱码 + "冷启动正常/热重启失效"的两个时序竞态（F1/F2）、三处修复、3 次连续验证 | ✅ 已修复并验证 |
| [PATH_D_DESIGN.md](../PATH_D_DESIGN.md) | 路径 D 总设计：让单字节渲染引擎显示 GBK 双字节汉字（字形索引拓宽到 16 位） | ✅ 已实机验证 |
| [HardTruckApocalypse_CHINESE_DLL/README.md](../HardTruckApocalypse_CHINESE_DLL/README.md) | 汉化插件（`hta_chs.asi`）实现细节 | — |
| [Render9Fix/README.md](../Render9Fix/README.md) | 崩溃修复插件（`Render9Fix.asi`）设计、安全设计、配置、日志判读 | — |

截图在 [screenshots/](screenshots/)。

---

## 两个插件的关系

| 插件 | 部署位置 | 职责 | 互扰 |
|---|---|---|---|
| `hta_chs.asi` | `update\` | 汉化：扩字形表到 16 位索引、挂图集、装配汉字 | 只在自己分配/释放的指针上活动 |
| `Render9Fix.asi` | `update\` | 把 dxrender9 的小对象分配导向 2GB 以下，消除 LAA 崩溃 | 只替换 Kernel 的 3 个分配器函数指针；`hta_chs` 走的是**直接函数指针**，不受影响 |

两者都是**运行时**补丁，不改 `hta.exe` 文件。

---

## 快速判据（看日志就知道成败）

| 日志 | 成功 | 失败 |
|---|---|---|
| `update\hta_chs.<时间戳>.log` | 约 **24 KB**（含大量"填充 2079 个汉字字形"） | 约 **5 KB**（停在初始化/弹框） |
| `update\Render9Fix.<时间戳>.log` | 有 `已知布局命中 … 完全一致 ✓` + `探针: 通过`；`高地址遗留` 只有几笔 ~131KB 纹理缓冲 | `等待超时` / `未通过校验` |
| `exmachina.log` | 无 `could not validate technique` | 有该行 = 又崩回 dxrender9 的 D3DX 路径 |

---

## 工具

| 工具 | 用途 |
|---|---|
| [tools/null_dbg.ps1](../tools/null_dbg.ps1) | 透明调试器：复刻 x32dbg 的"延迟治百病"现象，可精确注入每事件 N 毫秒延迟，并在崩溃瞬间抓 `[FX]` 取证 |
| [tools/null_dbg_check.ps1](../tools/null_dbg_check.ps1) | 上面那个调试器内嵌 C# 的秒级编译自检（不启动游戏；已自动处理 32 位/UTF-8 BOM 两个坑） |
| [tools/deploy_render9fix.ps1](../tools/deploy_render9fix.ps1) | Render9Fix 安装/卸载 |
| [tools/fxscan.py](../tools/fxscan.py) | 扫 fx 文件 technique 数量（找"单 technique"风险文件） |
| [tools/cmp_dumps.py](../tools/cmp_dumps.py) | 批量比对 minidump（寄存器/模块归属/栈返回地址） |
| [Render9Fix/test/](../Render9Fix/test/) | Render9Fix 的端到端合成测试（假 Kernel + 假 dxrender9.dll），一键 `build_and_run.ps1` |

## 游戏侧工具（存档）

本体与**资料片通用**，原始位置在游戏根目录，现存档于 [dist/](../dist/)：

| 文件 | 用途 |
|---|---|
| `X86Game4gb.exe` | 给 32 位 PE 打 LAA（大地址感知）补丁；源码在 [X86Game4gb/](../X86Game4gb/) |
| `hta_【必装】扩展32位游戏内存分配来缓解崩溃问题.bat` | 上一项的一键封装（对 `hta.exe`） |
| `hta_单核运行游戏来解决老游戏不兼容问题.bat` | `/AFFINITY 0x2` 单核启动（**治不了 LAA 崩溃**，那个靠 Render9Fix） |
| `清除俄语输入法布局.ps1` / `.exe` | 卸载俄语键盘布局 KLID `00000419` |

资料片（`Meridian113.exe`）用法与差异见 [dist/README.md](../dist/README.md)。

---

## 复现崩溃用的调试档位（实测）

| 配置 | 结果 |
|---|---|
| 无调试器 + LAA | 崩（约 100% 进世界时） |
| `null_dbg.ps1 -Quality Fast`（0 延迟） | 约 1/2 概率崩 |
| `-DelayMs 3` | 不稳定（有时崩） |
| `-DelayMs 4` | 基本稳定不崩 |
| `-DelayMs 5` | 很稳 |
| 单核 + LAA + 无调试器 | **仍崩**（这条排除了并发/双核竞态） |
