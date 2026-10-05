@echo off
REM build_tools.bat ?? ???????? sigtest.exe
REM ???? VS ???????????? cmd ??????
setlocal

set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
set HERE=%~dp0
set SRC=%HERE%..
set OUT=%HERE%sigtest.exe

call %VCVARS% x86 >nul 2>&1
if errorlevel 1 (
  echo [??] vcvarsall.bat ????
  exit /b 1
)

pushd "%HERE%"
cl /nologo /W3 /MT /EHsc /D_CRT_SECURE_NO_WARNINGS ^
   sigtest.cpp "%SRC%\pattern.cpp" "%SRC%\ldisasm.cpp" ^
   /Fe:"%OUT%" /Fo:"%HERE%" /link /SUBSYSTEM:CONSOLE
set RC=%errorlevel%
popd

if %RC% neq 0 (
  echo [??] ????
  exit /b %RC%
)
echo [??] %OUT%
exit /b 0
