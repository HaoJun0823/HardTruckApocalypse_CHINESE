@echo off
REM run_tests.bat -- offline validation for the DLC1 plugin (no game launch)
setlocal
set HERE=%~dp0
call "%HERE%build_tools.bat"
if errorlevel 1 exit /b 1
"%HERE%sigtest.exe" "I:\LocalGames\Hard Truck Apocalypse STEAM\hta.exe" "I:\LocalGames\HARD TRUCK APOCALYPSE RISE OF CLANS STEAM\Meridian113.exe"
exit /b %errorlevel%
