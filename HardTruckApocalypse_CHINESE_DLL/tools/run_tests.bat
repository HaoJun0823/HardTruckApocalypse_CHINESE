@echo off
REM run_tests.bat -- run all offline verification (no game launch needed)
REM NOTE: keep this file ASCII-only; it is written with CRLF.
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
set SRC=%~dp0..
set TOOLS=%~dp0
set GAME1=I:\LocalGames\Hard Truck Apocalypse STEAM\hta.exe
set GAME2=I:\LocalGames\HARD TRUCK APOCALYPSE RISE OF CLANS STEAM\Meridian113.exe

call %VCVARS% x86 >nul 2>&1
if errorlevel 1 ( echo [ERROR] vcvarsall.bat failed & exit /b 1 )

pushd "%TOOLS%"

echo.
echo ============ 1/2  sigtest (signature scan) ============
cl /nologo /W3 /MT /EHsc /D_CRT_SECURE_NO_WARNINGS /utf-8 sigtest.cpp "%SRC%\pattern.cpp" "%SRC%\ldisasm.cpp" /Fe:sigtest.exe /link /SUBSYSTEM:CONSOLE >nul
if errorlevel 1 ( echo [ERROR] sigtest build failed & popd & exit /b 1 )
sigtest.exe "%GAME1%" "%GAME2%"
if errorlevel 1 ( echo [FAIL] signature scan failed & popd & exit /b 1 )

echo.
echo ============ 2/2  transcode unit test ============
cl /nologo /W3 /MT /EHsc /D_CRT_SECURE_NO_WARNINGS /utf-8 test_transcode.cpp "%SRC%\transcode.cpp" "%SRC%\slotmap.cpp" "%SRC%\log.cpp" "%SRC%\pattern.cpp" "%SRC%\font_hooks.cpp" "%SRC%\ldisasm.cpp" "%SRC%\text_hooks.cpp" "%SRC%\hook.cpp" /Fe:test_transcode.exe /link /SUBSYSTEM:CONSOLE >nul
if errorlevel 1 ( echo [ERROR] test_transcode build failed & popd & exit /b 1 )

set "MAP=%SRC%\..\update_chs_test\hta_chs_slotmap.txt"
if not exist "%MAP%" (
  echo [SKIP] slotmap not found: %MAP%
) else (
  test_transcode.exe "%MAP%"
  if errorlevel 1 ( echo [FAIL] transcode test failed & popd & exit /b 1 )
)

popd
echo.
echo ============ ALL OFFLINE TESTS PASSED ============
exit /b 0
