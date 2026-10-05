@echo off
REM build.bat ?? ?? hta_chs.dll?Release / Win32?
REM ???build\Release\hta_chs.dll
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
set PROJ=%~dp0HardTruckApocalypse_CHINESE_DLL.vcxproj

call %VCVARS% x86 >nul 2>&1
if errorlevel 1 ( echo [??] vcvarsall.bat ?? & exit /b 1 )

%MSBUILD% "%PROJ%" /p:Configuration=Release /p:Platform=Win32 /v:minimal /nologo
if errorlevel 1 ( echo [??] ???? & exit /b 1 )

echo.
echo [??] %~dp0build\Release\hta_chs.dll
exit /b 0
