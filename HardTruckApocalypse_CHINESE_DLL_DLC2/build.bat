@echo off
REM build.bat -- one-click build of the DLC2 Chinese plugin (Release / Win32)
REM Output: build\Release\hta_chs_dlc2.dll  -> rename to hta_chs_dlc2.asi when deploying
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
set PROJ=%~dp0HardTruckApocalypse_CHINESE_DLL_DLC2.vcxproj

call %VCVARS% x86 >nul 2>&1
if errorlevel 1 ( echo [ERROR] vcvarsall.bat failed & exit /b 1 )

%MSBUILD% "%PROJ%" /p:Configuration=Release /p:Platform=Win32 /v:minimal /nologo
if errorlevel 1 ( echo [ERROR] build failed & exit /b 1 )

echo.
echo [OK] %~dp0build\Release\hta_chs_dlc2.dll
exit /b 0
