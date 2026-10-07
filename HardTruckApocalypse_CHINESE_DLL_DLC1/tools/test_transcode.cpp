// test_transcode.cpp —— 转码逻辑的离线单测
//
// 不注入游戏，直接编译真实的 transcode.cpp / slotmap.cpp / text_hooks.cpp，
// 验证：
//   1. 加载随字库生成的槽位映射表
//   2. GbkToSlots：GBK 串 -> [0x1B][槽位] 序列
//   3. 等长性：输出字节数 == 输入字节数
//   4. TranscodeInPlace：就地替换后长度不变
//
// 构建（VS x86 开发命令行）：
//   cl /nologo /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
//     test_transcode.cpp ..\transcode.cpp ..\slotmap.cpp ..\log.cpp ^
//     ..\pattern.cpp ..\font_hooks.cpp ..\ldisasm.cpp /link /SUBSYSTEM:CONSOLE
//
// 运行：
//   test_transcode.exe <slotmap.txt>  "<GBK测试串>"
// 例：
//   test_transcode.exe hta_chs_slotmap.txt "新游戏"
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

// ---- 需要的最小声明 ----
namespace font {
bool IsLeadByte(uint8_t b);
void InitLeadByteTable();
}
namespace transcode {
bool GbkToSlots(const char* src, char* dst, size_t dstCap);
bool SelfTest(char* errBuf, size_t errCap);
size_t EscapeByte();
int SlotsPerFont();
}
namespace slotmap {
bool Load(const char* path);
bool Lookup(uint16_t gbk, uint8_t* slot);
int Count();
}
namespace texthook {
int TranscodeInPlace(char* p, size_t len);
}

int g_fail = 0;

static void CHECK(bool cond, const char* what) {
    if (!cond) { ++g_fail; printf("  [FAIL] %s\n", what); }
    else       { printf("  [ok]   %s\n", what); }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: test_transcode.exe <slotmap.txt> [GBK串]\n");
        return 2;
    }
    const char* mapPath = argv[1];
    const char* testStr = (argc >= 3) ? argv[2] : "\xD0\xC2"  // 新 (GBK)
                                       "\xD3\xCE"            // 游
                                       "\xCF\xB7";           // 戏

    printf("=== hta_chs 转码逻辑单测 ===\n");
    printf("slotmap: %s\n", mapPath);
    printf("测试串: [%s]\n", testStr);

    // 自检
    char err[256] = {0};
    CHECK(transcode::SelfTest(err, sizeof(err)), "transcode 自检");
    if (err[0]) printf("        %s\n", err);

    // 加载映射
    bool loaded = slotmap::Load(mapPath);
    CHECK(loaded, "加载 slotmap");
    printf("        %d 条映射\n", slotmap::Count());
    if (!loaded) return 1;

    // 解析测试串的 GBK 双字节
    const char* p = testStr;
    int pairs = 0;
    printf("测试串字节:");
    for (const uint8_t* q = (const uint8_t*)testStr; *q; ++q)
        printf(" %02X", *q);
    printf("\n");

    // 验证等长转码
    size_t inLen = 0;
    {
        const uint8_t* q = (const uint8_t*)testStr;
        while (q[inLen]) ++inLen;
    }
    char out[256] = {0};
    bool ok = transcode::GbkToSlots(testStr, out, sizeof(out));
    CHECK(ok, "GbkToSlots");
    size_t outLen = 0;
    {
        const uint8_t* q = (const uint8_t*)out;
        while (q[outLen]) ++outLen;
    }
    printf("        输入 %zu 字节, 输出 %zu 字节\n", inLen, outLen);
    CHECK(inLen == outLen, "等长转码（GBK 2字节 -> 转义2字节）");

    // 输出应只有 0x1B 与 0x80..0xFF
    bool clean = true;
    for (size_t i = 0; i < outLen; ++i) {
        uint8_t b = (uint8_t)out[i];
        if (i % 2 == 0) { if (b != transcode::EscapeByte()) { clean = false; } }
        else            { if (b < 0x80 || b >= 0x100) { clean = false; } }
    }
    CHECK(clean, "输出格式为 [ESC][槽位] 交替");
    if (outLen)
        printf("        输出十六进制: ");
    for (size_t i = 0; i < outLen; ++i) printf(" %02X", (uint8_t)out[i]);
    printf("\n");

    // 就地转码
    char inPlace[256] = {0};
    memcpy(inPlace, testStr, inLen);
    int n = texthook::TranscodeInPlace(inPlace, inLen);
    printf("        TranscodeInPlace 替换 %d 个汉字\n", n);
    CHECK(n == (int)(inLen / 2), "就地转码替换数 == GBK 序列数");
    CHECK(memcmp(inPlace, out, outLen) == 0, "就地转码结果与 GbkToSlots 一致");

    // 正向验证：测试串里每一对 GBK 都应能在映射表中查到槽号
    int mapped = 0;
    {
        const uint8_t* t = (const uint8_t*)testStr;
        for (size_t i = 0; i + 1 < inLen; i += 2) {
            uint16_t gbk = ((uint16_t)t[i] << 8) | t[i+1];
            uint8_t slot = 0;
            if (slotmap::Lookup(gbk, &slot)) ++mapped;
        }
    }
    CHECK(mapped == (int)(inLen / 2), "映射表中每对 GBK 都可查到槽号");

    printf("\n=== %s (%d 项失败) ===\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}