@echo off
setlocal

title Uninstall WinKeyHook

:: Self-elevate if not already admin
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Requesting admin rights...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"%~f0\"' -Verb RunAs"
    exit /b
)

echo ======================================================
echo    UNINSTALL - WinKeyHook Daemon
echo ======================================================
echo.

:: Kill the daemon
echo [1/4] Stopping WinKeyHook.exe...
taskkill /F /IM WinKeyHook.exe /T >nul 2>&1
timeout /t 1 /nobreak >nul

:: Run official uninstaller(s) found in registry
echo [2/4] Running uninstaller(s)...
powershell -ExecutionPolicy Bypass -Command ^
  "$keys = @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'); " ^
  "Get-ItemProperty $keys -ErrorAction SilentlyContinue | " ^
  "Where-Object { $_.DisplayName -like '*WinKeyHook*' } | " ^
  "ForEach-Object { $u = $_.UninstallString -replace '\"',''; if ($u -and (Test-Path $u)) { Write-Host ('  Running: ' + $u); Start-Process $u -ArgumentList '/SILENT' -Wait } }"
timeout /t 2 /nobreak >nul

:: Nuke any leftover registry keys (both old name-based and GUID-based AppId)
echo [3/4] Cleaning registry...
powershell -ExecutionPolicy Bypass -Command ^
  "$orphans = @(" ^
  "  'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\WinKeyHook Daemon_is1'," ^
  "  'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{B3F7A2D1-9C4E-4F8B-A6D5-2E1C0B5F8A3D}_is1'," ^
  "  'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\WinKeyHook Daemon_is1'," ^
  "  'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{B3F7A2D1-9C4E-4F8B-A6D5-2E1C0B5F8A3D}_is1'" ^
  "); $orphans | Where-Object { Test-Path $_ } | ForEach-Object { Remove-Item $_ -Force; Write-Host ('  Removed: ' + $_) }"

:: Delete leftover install folder
echo [4/4] Removing files...
powershell -ExecutionPolicy Bypass -Command ^
  "$folders = @('C:\Program Files\Ephraem\Daemons','C:\Program Files (x86)\Ephraem\Daemons'); " ^
  "$folders | Where-Object { Test-Path $_ } | ForEach-Object { Remove-Item $_ -Recurse -Force -ErrorAction SilentlyContinue; Write-Host ('  Deleted: ' + $_) }"

echo.
echo ======================================================
echo    DESINSTALLATION TERMINEE
echo ======================================================
echo.
pause
