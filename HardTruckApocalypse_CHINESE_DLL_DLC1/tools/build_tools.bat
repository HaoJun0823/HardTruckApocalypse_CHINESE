@echo off
REM build_tools.bat -- build sigtest.exe (offline anchor scanner, no game launch)
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
set HERE=%~dp0
set SRC=%HERE%..
call %VCVARS% x86 >nul 2>&1
if errorlevel 1 ( echo [ERROR] vcvarsall.bat failed & exit /b 1 )
pushd "%HERE%"
cl /nologo /W3 /MT /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS sigtest.cpp "%SRC%\pattern.cpp" "%SRC%\ldisasm.cpp" /Fe:"%HERE%sigtest.exe" /link /SUBSYSTEM:CONSOLE
set RC=%errorlevel%
popd
if %RC% neq 0 ( echo [ERROR] sigtest build failed & exit /b %RC% )
echo [OK] %HERE%sigtest.exe
exit /b 0
