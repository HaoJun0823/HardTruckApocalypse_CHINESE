@echo off
REM build.bat -- build DLC2_MemFix DLL (Release / Win32)
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
set PROJ=%~dp0DLC2_MemFix.vcxproj
call %VCVARS% x86 >nul 2>&1
if errorlevel 1 ( echo [ERROR] vcvarsall.bat failed & exit /b 1 )
%MSBUILD% "%PROJ%" /p:Configuration=Release /p:Platform=Win32 /v:minimal /nologo
if errorlevel 1 ( echo [ERROR] build failed & exit /b 1 )
echo [OK] %~dp0build\Release\DLC2_MemFix.dll
exit /b 0
