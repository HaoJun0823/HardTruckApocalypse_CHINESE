// Clean_RUS_Layout.cpp
// 作用：卸载系统中所有俄语键盘布局。

#include <windows.h>
#include <iostream>
#include <vector>
#include <iomanip>

#pragma comment(lib, "user32.lib")

// 判断某个 HKL 是否为俄语布局
static bool IsRussianLayout(HKL hkl)
{
	// HKL 的低 16 位是语言 ID，高 16 位是设备句柄
	WORD langId = LOWORD(reinterpret_cast<DWORD_PTR>(hkl));
	return PRIMARYLANGID(langId) == LANG_RUSSIAN;
}

int wmain()
{
	// 1. 枚举当前所有已加载的键盘布局
	int count = GetKeyboardLayoutList(0, nullptr);
	if (count <= 0)
	{
		std::wcerr << L"无法获取键盘布局列表，错误码: " << GetLastError() << L"\n";
		return 1;
	}

	std::vector<HKL> layouts(count);
	count = GetKeyboardLayoutList(count, layouts.data());
	if (count <= 0)
	{
		std::wcerr << L"无法获取键盘布局列表，错误码: " << GetLastError() << L"\n";
		return 1;
	}

	// 2. 分离出俄语布局和一个非俄语的备用布局
	std::vector<HKL> russianLayouts;
	HKL fallback = nullptr;

	for (int i = 0; i < count; ++i)
	{
		if (IsRussianLayout(layouts[i]))
			russianLayouts.push_back(layouts[i]);
		else if (fallback == nullptr)
			fallback = layouts[i];
	}

	if (russianLayouts.empty())
	{
		std::wcout << L"系统中没有找到俄语键盘布局。\n";
		return 0;
	}

	if (fallback == nullptr)
	{
		std::wcerr << L"系统中除俄语外没有任何其他键盘布局，无法卸载。\n";
		return 1;
	}

	// 3. 如果当前线程正处于俄语布局，先切换到备用布局
	HKL current = GetKeyboardLayout(0);
	if (IsRussianLayout(current))
	{
		ActivateKeyboardLayout(fallback, 0);
		std::wcout << L"当前处于俄语布局，已切换到备用布局。\n";
	}

	// 4. 逐个卸载俄语布局
	int failed = 0;
	for (HKL hkl : russianLayouts)
	{
		DWORD_PTR raw = reinterpret_cast<DWORD_PTR>(hkl);
		if (UnloadKeyboardLayout(hkl))
		{
			std::wcout << L"已卸载俄语布局，HKL = 0x"
				<< std::hex << raw << std::dec << L"\n";
		}
		else
		{
			DWORD err = GetLastError();
			std::wcerr << L"卸载失败，HKL = 0x"
				<< std::hex << raw << std::dec
				<< L"，错误码: " << err
				<< L" (0x" << std::hex << err << std::dec << L")\n";
			++failed;
		}
	}

	if (failed == 0)
		std::wcout << L"全部俄语布局已清理完成。\n";
	else
		std::wcout << L"完成，但有 " << failed << L" 个布局卸载失败。\n";

	return failed == 0 ? 0 : 1;
}