if (-not ("KeyboardLayoutHelper" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class KeyboardLayoutHelper
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadKeyboardLayout(string pwszKLID, uint Flags);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool UnloadKeyboardLayout(IntPtr hkl);

    public const uint KLF_NOTELLSHELL = 0x00000080;
}
"@
}

$RU_KLID = "00000419"

# 用 LoadKeyboardLayout 拿到已存在的俄语布局句柄（不会新增重复项）
$hklRu = [KeyboardLayoutHelper]::LoadKeyboardLayout($RU_KLID, [KeyboardLayoutHelper]::KLF_NOTELLSHELL)
if ($hklRu -eq [IntPtr]::Zero) {
    Write-Host "系统中没有找到俄语布局。"
    return
}

# 卸载
$ok = [KeyboardLayoutHelper]::UnloadKeyboardLayout($hklRu)
if ($ok) {
    Write-Host "俄语布局已卸载。"
} else {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Warning "卸载失败。错误码: $err （0x57=参数错; 0x1F=设备/布局正在使用）"
}