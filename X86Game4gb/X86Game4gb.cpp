// 4gb.cpp — 给 32 位 PE 打 LARGE_ADDRESS_AWARE 补丁
// 编译: cl /nologo /EHsc /O2 /W4 /utf-8 4gb.cpp
// 用法: 4gb.exe <目标.exe>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// 兼容旧版 MSVC + 新版 Windows SDK 的 "unknown attribute no_init_all"
// 把 _SAL_VERSION 压回 18, winnt.h 就不会展开那个新注解
#if defined(_MSC_VER) && _MSC_VER < 1930
#  ifndef _SAL_VERSION
#    define _SAL_VERSION 18
#  endif
#endif

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>

// ================= 备份 =================
static bool BackupFile(const std::wstring& path, std::wstring& outBak)
{
	for (int i = 0; i < 1000; ++i)
	{
		std::wstring candidate = path + L".bak";
		if (i > 0)
		{
			wchar_t suffix[32];
			swprintf_s(suffix, L".bak.%d", i);
			candidate = path + suffix;
		}
		if (CopyFileW(path.c_str(), candidate.c_str(), TRUE))
		{
			outBak = candidate;
			return true;
		}
		DWORD err = GetLastError();
		if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
		{
			wprintf(L"[x] 备份失败, 错误码: %lu\n", err);
			return false;
		}
	}
	wprintf(L"[x] 备份失败: 同名备份过多\n");
	return false;
}

// ================= PE 内存打补丁 =================
// 注意: 本函数只能出现 POD 类型, 不能有 std::wstring / std::vector 等需要析构的对象,
//       否则 MSVC 会报 C2712 (无法在要求对象展开的函数中使用 __try)
struct PeResult { int status; BOOL is64; };
// status: 0 = 已写入, 1 = 本来就有, 2 = 非有效 PE / 发生异常

static PeResult PatchPeInMemory(BYTE* base, unsigned long long fileSize)
{
	PeResult r{ 2, FALSE };
	__try
	{
		auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return r;

		LONG lfanew = dos->e_lfanew;
		if (lfanew <= 0 ||
			static_cast<unsigned long long>(lfanew) + sizeof(IMAGE_NT_HEADERS32) > fileSize)
			return r;

		auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return r;

		WORD machine = nt->FileHeader.Machine;
		r.is64 = (machine == IMAGE_FILE_MACHINE_AMD64 ||
			machine == IMAGE_FILE_MACHINE_IA64 ||
			machine == IMAGE_FILE_MACHINE_ARM64);

		if (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)
		{
			r.status = 1;
			return r;
		}
		nt->FileHeader.Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
		r.status = 0;
		return r;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		r.status = 2;
		r.is64 = FALSE;
		return r;
	}
}

// ================= 主逻辑 =================
static int PatchFile(const std::wstring& path)
{
	std::wstring bak;
	if (!BackupFile(path, bak))
		return 2;
	wprintf(L"[+] 已备份到: %s\n", bak.c_str());

	HANDLE hFile = CreateFileW(path.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		wprintf(L"[x] 打开文件失败, 错误码: %lu\n", GetLastError());
		return 3;
	}

	LARGE_INTEGER fileSize{};
	if (!GetFileSizeEx(hFile, &fileSize) ||
		fileSize.QuadPart < (LONGLONG)sizeof(IMAGE_DOS_HEADER))
	{
		wprintf(L"[x] 文件过小或读取大小失败\n");
		CloseHandle(hFile);
		return 3;
	}

	HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
	if (!hMap)
	{
		wprintf(L"[x] 创建文件映射失败, 错误码: %lu\n", GetLastError());
		CloseHandle(hFile);
		return 3;
	}

	BYTE* base = static_cast<BYTE*>(MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0));
	if (!base)
	{
		wprintf(L"[x] 映射视图失败, 错误码: %lu\n", GetLastError());
		CloseHandle(hMap);
		CloseHandle(hFile);
		return 3;
	}

	PeResult r = PatchPeInMemory(base, static_cast<unsigned long long>(fileSize.QuadPart));

	int rc = 3;
	switch (r.status)
	{
	case 0:
		if (FlushViewOfFile(base, 0))
		{
			if (r.is64)
				wprintf(L"[!] 这是 64 位程序, 本不需要 LAA, 已顺手设置\n");
			wprintf(L"[+] 已成功设置 LARGE_ADDRESS_AWARE\n");
			rc = 0;
		}
		else
		{
			wprintf(L"[x] 刷写文件失败, 错误码: %lu\n", GetLastError());
		}
		break;

	case 1:
		wprintf(L"[=] 目标已设置 LARGE_ADDRESS_AWARE, 无需修改\n");
		rc = 0;
		break;

	default:
		wprintf(L"[x] 不是有效的 32 位 PE, 或解析异常\n");
		break;
	}

	UnmapViewOfFile(base);
	CloseHandle(hMap);
	CloseHandle(hFile);
	return rc;
}

// ================= 入口 =================
int wmain(int argc, wchar_t** argv)
{
	if (argc != 2)
	{
		wprintf(L"用法: %s <目标可执行文件.exe>\n", argv[0]);
		wprintf(L"示例: %s .\\hta.exe\n", argv[0]);
		return 1;
	}

	std::wstring path = argv[1];

	DWORD attr = GetFileAttributesW(path.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
	{
		wprintf(L"[x] 找不到文件或该路径是目录: %s\n", path.c_str());
		return 1;
	}

	bool wasReadOnly = (attr & FILE_ATTRIBUTE_READONLY) != 0;
	if (wasReadOnly)
		SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);

	int rc = PatchFile(path);

	if (wasReadOnly)
		SetFileAttributesW(path.c_str(), attr);

	return rc;
}