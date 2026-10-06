# 路径 D 设计（IDA 反编译实证版 · 2026 修订）

> 本文所有结论都来自 IDA 对 `hta.exe` 的反编译/反汇编，括号内为地址。
> 无行号支撑的一律标注 **推测**。
>
> **本文档修订过一次**：早期版本把 `glyph+0x10` 当成「图集页号」、
> 把 `glyph+0x08` 当成「width」，这两条**都是错的**，见第 5 节。

---

## 1. Font 对象布局

依据：析构函数 `sub_8B67A0`（它显式写 `*(this+12)=*(this+48)=0` 等），
构造函数 `sub_8B6740`，绘制 `0x6865A6`、绑定 `0x6868CB`。

| 偏移 | 类型 | 含义 | 证据 |
|---|---|---|---|
| `+0x00` | 小字符串 | 字体名（SSO，16 字节） | `sub_8B8134 sub_406F50(a1, name)` |
| `+0x0C` | 小字符串 | 图集文件名（`_height`） | `sub_8B81D8` |
| `+0x18` | float | 字号 height | `0x8B8216 *(float*)(a1+24)=atof(...)` |
| `+0x1C` | float | heightVirtual | `0x8B833A sub_41D200(a1+28,...)` |
| `+0x20` | float | **缩放系数**（finalize 用） | `0x8B92B7 *(a1+32)=1.0f` |
| `+0x24` | int | `==1` 表示「已 finalize」 | `0x8B92B0 *(a1+36)=1` |
| `+0x30` | void** | 图集页数组 **begin** | `0x6868CB mov ecx,[edi+30h]` |
| `+0x34` | void** | 图集页数组 **end** | `0x6868D6 mov eax,[edi+34h]` |
| `+0x38` | void** | 图集页数组 **cap** | `sub_8B67A0 this+14=0` |
| `+0x40` | void** | **字形表基址**（256 项） | `0x6865A6 mov edx,[edi+40h]` |
| `+0x44` | void** | 字形表 **end** | `sub_8B67A0 this+17` |
| `+0x48` | int | **字形表当前容量（项数）** | `0x8B8BC1 v24=*(DWORD*)(a1+64)` |

**★ 页数组的元素是 `texId`（int），不是指针。**
`0x8B8542`：`renderer->vfunc[1000](renderer, texIdSlot, 0, 2)` —— 传的是
`*(_DWORD*)(a1+48) + 4*v17 - 4`，即数组里存的整数本身。

---

## 5. 字形结构（48 字节）

三重核实：`sub_8B80B0` 逐字段初始化 + `sub_8B6280` finalize + `fonts.xml` 实测数据。

| 偏移 | 类型 | 含义 | 证据 |
|---|---|---|---|
| `+0x00` | u8 | 字符码（引擎只拿它做字典校验） | `0x8B8A68` |
| `+0x04` | f32 | abc[0] | `0x8B8AE7` |
| `+0x08` | f32 | abc[1] | `0x8B8AF4` |
| `+0x0C` | f32 | abc[2] | `0x8B8B01` |
| `+0x10` | i32 | **图集页号** | `0x6868C7 test edx,edx / jl` |
| `+0x14` | f32 | u0 | `0x686693` |
| `+0x18` | f32 | v0 | `0x68669A` |
| `+0x1C` | f32 | u1 | `0x6866A4` |
| `+0x20` | f32 | v1 | `0x6866A7` |
| `+0x24` | f32 | 纹素宽（像素） | `0x8B62D8 *(_DWORD*)(*v7+36)` |
| `+0x28` | f32 | 纹素高（像素） | `0x8B62E0 v9[1]` |
| `+0x2C` | f32 | advance = abc0+abc1+abc2 | `0x8B6337` |

### ★ 决定性证据：`+0x10` 是页号（我一度搞错过）★

`fonts.xml` 实测：

```
value='!'  abc="1.000 2.000 2.000"  tcs="0.000 0.016 0.000 0.031 0.078"
```

绘制端 `0x686689` 读 `+0x10..+0x20` 五个连续 float，其中 `+0x10`（`edx`）
在 `0x6868C7` 被 `test edx,edx / jl` 当**页号**校验，再在 `0x6868DE` 与
`(Font+0x34 - Font+0x30)>>2`（页数）比较。

把 tcs 五个值映射到 `+0x10..+0x20`：页号=0.000、u0=0.016、v0=0.000、u1=0.031、v1=0.078
—— u/v 全落在 0..1，语义自洽。若改成「+0x10 是纹素宽」，u/v 会整体错位。
且全文件 `tcs[0]` 恒为 `0.0`（共 5824 个符号），单页字体，符合页号语义。

### finalize 只处理单字节

`sub_8B6280` 遍历 `off_A05E38`（单字节字符集），且 `sub_8B5AE0` 是在
**引擎自己的图集纹理**里按字符找位置。我们的汉字在独立图集，引擎无从得知，
所以 `+0x24/+0x28/+0x2C` 必须自己填。

---

## 3. 绘制主循环寄存器（`sub_685CA0`，已验证）

```
0x6864B1  mov esi, v55        ; esi = 下标
0x6864B4  v54 = v118[0]       ; eax = 文本基址
0x6864E0  mov bl, [esi+eax]   ; 当前字符
```

| 寄存器 | 语义 |
|---|---|
| `eax` | 文本基址 |
| `esi` | 下标 |
| `bl` | 当前字符字节 |
| `edi`（`0x6865A2`） | Font 对象 |

**取 GBK 第 2 字节 = `[esi+eax+1]`**，读它安全（末字节是 `\0`）。

---

## 4. 两处查表

| 位置 | 覆盖 | 取什么 |
|---|---|---|
| P4 `0x6865A6` | 18 字节 | `eax`=字形指针，`ecx`=槽地址 |
| P4b `0x686A26` | 10 字节 | `eax`=字形指针 |
| P5 `0x686A55` | 6 字节 | 推进 1 或 2 字节 |

P4 长度 = `3+3+2+2+3+3+2 = 18`（逐条累加，不许手算 hex）。

---

## 5. 纹理接口（renderer 虚表，`dword_A0B55C+764` 是 renderer）

| 虚表偏移 | 调用约定 | 用途 | 出处 |
|---|---|---|---|
| `+0x3B0` (944) | `__thiscall (r, &out, filename, flags)` | **加载纹理文件** | `0x8B84FB` |
| `+0x3B4` (948) | `__thiscall (r, &out, "$FontTex", w, h, 4)` | 建空白纹理 | `0x8B99DC` |
| `+0x3C8` (968) | `__thiscall (r, texIdSlot)` | 解绑/释放 | `0x8B67E1` |
| `+0x3E8` (1000) | `__thiscall (r, texIdSlot, 0, 2)` | 绑定 | `0x8B8542` |
| `+0x3CC` (972) | `__thiscall (r, ?, texId)` | 绘制时绑定 | `0x68692C` |

---

## 6. 引擎 XML 加载流程（`sub_8B80B0`，已完整反编译）

```
sub_8B67A0(font)                        // 清空
node->GetAttr("name")     → Font+0x00
sub_8B4A70(...)            → Font+0x0C   (拼 "名字_字号")
node->GetAttr("height")   → Font+0x18
node->GetAttr("heightVirtual") → Font+0x1C
node->GetAttr("file")     → 拆分多路径
for each file:  renderer->Load(+0x3B0) ; pages.push_back(texId)
for each <Symbol>:
    value  长度必须 == 1        ★★★ 单字节硬限制在这里 ★★★
    alloc(48) → 填 abc / tcs
    table[value[0]] = glyph
Font+0x24 = 1 ; Font+0x20 = 1.0
sub_8B6280(font)                      // finalize：算 +0x10/+0x14/+0x2C
```

**源码级证据**：`0x8B872E if (!v72 || strlen(v72) != 1)` →
`"Font::CreateFromXmlNode error: invalid symbol name"`。
这就是「游戏只支持单字节」的**确凿出处**。

---

## 7. 方案 C 的实现要点

| 步骤 | 做法 |
|---|---|
| 挂钩 | `sub_8B80B0`，`__userpurge(a1@ecx, a2@edi)` —— ★ 必须原样保留 `edi` ★ |
| 时机 | 在 trampoline 返回**之后**（引擎已 finalize，advance 已算好） |
| 加载纹理 | 用 `renderer->vfunc[0x3B0]` 逐页加载 `cjk_*.dds` |
| 挂页 | push 进 `Font+0x30` 数组（begin/end/cap 三个指针） |
| 填字形 | 自己 `alloc(48)`，填 `abc/tcs`，索引 = `b1|(b2<<8)`，写进 `g_cjkTable` |
| advance | 自己算 `cellW`（不要指望 finalize，它只处理 `off_A05E38` 里的单字节集） |

### 为什么不能直接复用引擎的 finalize

`sub_8B6280` 只遍历 `off_A05E38` 这个**单字节字符集**，
且 `sub_8B5AE0` 是去引擎自己的图集纹理里按字符找位置。
我们的汉字在独立图集里，引擎无从得知，所以 `+0x10/+0x14/+0x2C` 必须自己算。

---

## 8. 当前状态

| 项目 | 状态 |
|---|---|
| 崩溃 | ✅ 已解决 |
| ASCII 渲染 | ✅ 正常 |
| GBK 双字节取字 | ✅ 已实现（`[esi+eax+1]`），实测不崩 |
| 纹理页挂载 | ⏳ 方案 C 实现中 |
| 中文宽度 | ❌ 度量函数 `sub_685990` 未补双字节推进 |