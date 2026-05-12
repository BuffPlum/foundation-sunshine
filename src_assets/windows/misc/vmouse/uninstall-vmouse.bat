@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
chcp 65001 >nul
setlocal enabledelayedexpansion

rem ============================================================================
rem  Zako Virtual Mouse Driver - Uninstallation Script
rem ============================================================================

rem Get sunshine root directory
for %%I in ("%~dp0..\..") do set "ROOT_DIR=%%~fI"

set "DIST_DIR=%ROOT_DIR%\tools\vmouse"
set "NEFCON=%ROOT_DIR%\tools\nefconw.exe"

rem Check if nefconw.exe exists
if not exist "%NEFCON%" (
    set "NEFCON=%ROOT_DIR%\tools\vdd\nefconw.exe"
)

rem Stop Sunshine service to release HID device handle.
rem Use sc + taskkill instead of `net stop` to avoid up to 30s SCM blocking.
echo Stopping Sunshine service...
set "SERVICE_WAS_RUNNING=0"
sc query SunshineService >nul 2>&1
if not errorlevel 1 (
    rem Service exists; check if it's running
    sc query SunshineService | find /I "RUNNING" >nul 2>&1
    if not errorlevel 1 (
        set "SERVICE_WAS_RUNNING=1"
        sc stop SunshineService >nul 2>&1
    )
    rem Force-kill the service binary so it releases the HID handle quickly.
    taskkill /f /im sunshinesvc.exe >nul 2>&1
    timeout /t 1 /nobreak >nul 2>&1
    echo Sunshine service stopped.
) else (
    echo Sunshine service not installed, OK.
)

if not exist "%NEFCON%" goto skip_nefcon_uninstall

echo Removing all Virtual Mouse devices via nefcon...
set "NEFCON_REMOVED=0"
set "NEFCON_MAX_ITERS=20"
:uninstall_remove_loop
"%NEFCON%" --remove-device-node --hardware-id Root\ZakoVirtualMouse --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da
if not errorlevel 1 (
    set /a NEFCON_REMOVED+=1
    rem Hard cap to prevent an infinite loop if nefcon reports success without
    rem actually removing anything (observed on some nefcon builds).
    if !NEFCON_REMOVED! GEQ !NEFCON_MAX_ITERS! (
        echo Reached max remove iterations (!NEFCON_MAX_ITERS!), stopping.
        goto after_remove_loop
    )
    timeout /t 1 /nobreak >nul
    goto uninstall_remove_loop
)
:after_remove_loop
echo Removed !NEFCON_REMOVED! device node(s) via nefcon.

echo Uninstalling Virtual Mouse driver...
"%NEFCON%" --uninstall-driver --inf-path "%DIST_DIR%\ZakoVirtualMouse.inf"
:skip_nefcon_uninstall

rem Fallback: use pnputil to remove any remaining device instances
rem This catches ghost devices that nefcon may fail to handle
echo Checking for remaining Virtual Mouse devices...
set "PNPUTIL_REMOVED=0"
for /f "tokens=*" %%d in ('powershell -NoProfile -Command ^
    "Get-PnpDevice -InstanceId 'ROOT\HIDCLASS\*' -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\ZakoVirtualMouse' } | ForEach-Object { $_.InstanceId }"') do (
    echo Removing remaining device: %%d
    pnputil /remove-device "%%d" >nul 2>&1
    set /a PNPUTIL_REMOVED+=1
)
if !PNPUTIL_REMOVED! GTR 0 (
    echo Removed !PNPUTIL_REMOVED! remaining device^(s^) via pnputil.
) else (
    echo No remaining devices found.
)

rem Clean up driver package from DriverStore (locale-independent)
powershell -NoProfile -Command "Get-ChildItem ($env:SystemRoot + '\INF\oem*.inf') -ErrorAction SilentlyContinue | Where-Object { Select-String -Path $_.FullName -Pattern 'ZakoVirtualMouse' -Quiet } | ForEach-Object { Write-Host ('Removing driver package: ' + $_.Name); pnputil /delete-driver $_.Name /force | Out-Null }"

rem Clean up files
if exist "%DIST_DIR%" (
    rmdir /S /Q "%DIST_DIR%"
)

echo Virtual Mouse driver uninstalled.

rem Restart Sunshine service if it was running before and still exists.
rem In the full uninstaller flow the service has already been deleted by
rem uninstall-service.bat, so this only matters when the script is run
rem standalone (e.g. user-initiated driver reset).
if "!SERVICE_WAS_RUNNING!"=="1" (
    sc query SunshineService >nul 2>&1
    if not errorlevel 1 (
        echo Restarting Sunshine service...
        sc start SunshineService >nul 2>&1
    )
)

exit /b 0
