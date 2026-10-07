# dist —— 放到**游戏根目录**使用的工具（存档）

这 5 个文件原本直接躺在游戏根目录里（`I:\LocalGames\Hard Truck Apocalypse STEAM\`）。
放在这里是为了**留存与复用**：它们对本体 `hta.exe` 与资料片 `Meridian113.exe` 同样适用，
而本体/资料片的部署目录是分开的，工具本身却是通用的。

> 使用方式：把需要的文件复制回游戏根目录（或资料片目录），然后双击对应的 `.bat`。
> `X86Game4gb.exe` 与 `清除俄语输入法布局.exe` 是**独立小工具**，不依赖任何 ASI 插件。

---

## 1. `X86Game4gb.exe` —— LAA（大地址感知）补丁器

给 32 位 PE 打 `IMAGE_FILE_LARGE_ADDRESS_AWARE`，让进程能用满 4GB 用户地址空间。

```bat
X86Game4gb.exe <目标.exe>          :: 例：X86Game4gb.exe hta.exe
```

- 打补丁前会**自动备份**成 `<目标.exe>.bak.N`（N 递增，备份过多会拒绝执行）
- 只改 PE 头 `FileHeader.Characteristics` 里的 `0x0020` 位，不动其它任何字节
- 已打过补丁的文件会直接提示并跳过
- 源码：仓库 `X86Game4gb/`（本 exe 与 `X86Game4gb/Release/X86Game4gb.exe` 哈希一致：
  `3E1DA3B17313C2027AEA649F11B087F7D7362F3D33824140163E667C58625292`）

**为什么必装**：不挂 LAA 时，32 位进程的用户地址空间只有 2GB —— 引擎+汉化字库一起挤在这儿，
装配汉字图集时会因分配失败而崩。挂了 LAA 才有 4GB 可用。

---

## 2. `hta_【必装】扩展32位游戏内存分配来缓解崩溃问题.bat`

就是第 1 项的一键封装（**原始文件名里带"必装"，保留原名以对应玩家手里的版本**）：

```bat
cd %~dp0
call .\X86Game4gb.exe .\hta.exe
pause
```

---

## 3. `hta_单核运行游戏来解决老游戏不兼容问题.bat`

```bat
cd %~dp0
start /AFFINITY 0x2 hta
```

`/AFFINITY 0x2` = 只允许跑在 **CPU 序号 1**（第二颗逻辑核）上 —— 老引擎对多核调度敏感时的
兜底手段。`hta` 不带扩展名，由命令解释器补 `.exe`。

> 注意：这个手段**不能**解决 LAA 崩溃 —— 实测「单核 + LAA + 无调试器」照样崩在
> `dxrender9+0x416D6`（见 [LAA 崩溃分析](../docs/2026-10-07-LAA内存扩展崩溃-根因与Render9Fix.md)）。
> 那个问题由 [Render9Fix](../Render9Fix/README.md) 解决。

---

## 4/5. `清除俄语输入法布局.ps1` / `.exe`

游戏/系统会带进俄语键盘布局（KLID `00000419`），偶尔干扰输入。这个工具把它卸载掉：

```powershell
powershell -ExecutionPolicy Bypass -File 清除俄语输入法布局.ps1
```

实现：`LoadKeyboardLayout("00000419", KLF_NOTELLSHELL)` 拿到**已存在**的布局句柄
（不会新增重复项）→ `UnloadKeyboardLayout`。`.exe` 是同一功能的编译版（无需 PowerShell）。

---

## 资料片（`Meridian113.exe`）怎么用

同一套工具，只把目标名换掉：

```bat
:: 1) LAA 补丁（备份会自动生成为 Meridian113.exe.bak.N）
X86Game4gb.exe Meridian113.exe

:: 2) 单核启动
start /AFFINITY 0x2 Meridian113

:: 3) 俄语布局清理：与本体完全相同
```

两点差异要知道（详见 [PATH_D_DESIGN.md §10](../PATH_D_DESIGN.md)）：

| 项 | 本体 `hta.exe` | 资料片 `Meridian113.exe` |
|---|---|---|
| LAA 补丁 / 单核 / 俄语清理 | ✅ | ✅ 同一套工具 |
| 汉化 ASI | `hta_chs.asi` | ❌ **不能直接复用**：地址不同，需要独立的补丁集（`HardTruckApocalypse_CHINESE_DLL_DLC1/`，目前只有脚手架） |
| 字体引擎特征码 | ✅ | ✅ 同源，`HardTruckApocalypse_CHINESE_DLL/pattern.cpp` 就是为此用特征码而非硬编码地址 |

---

## 本体当前推荐的完整组合

| 组件 | 位置 | 作用 |
|---|---|---|
| LAA 补丁 | 本目录的工具 | 给 32 位进程 4GB 地址空间（汉化与引擎都要） |
| `Render9Fix.asi` | 游戏 `update\` | 把 dxrender9 的小对象分配导向 2GB 以下 —— 让 LAA 不再触发 `dxrender9+0x416D6` 崩溃 |
| `hta_chs.asi` | 游戏 `update\` | 汉化本体（16 位字形索引 + 图集装配） |
