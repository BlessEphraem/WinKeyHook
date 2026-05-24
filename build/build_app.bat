@echo off
setlocal enabledelayedexpansion

set "APP_NAME=WinKeyHook"
set "RELEASES_DIR=..\releases"

title Build %APP_NAME%

echo ======================================================
echo    BUILD SYSTEM - %APP_NAME%
echo ======================================================
echo.

set /p APP_VERSION="Version a generer (ex: 1.0.0) : "
if "%APP_VERSION%"=="" (
    echo [ERREUR] Version invalide.
    set /p DUMMY="Appuyez sur Entree pour quitter..."
    exit /b 1
)

if not exist "%RELEASES_DIR%" mkdir "%RELEASES_DIR%"

echo.
echo [1/3] Injection de la version (%APP_VERSION%) dans les sources...
python -c "import re, sys; f='../src/WinKeyHook.cpp'; c=open(f, encoding='utf-8').read(); c=re.sub(r'WINKEYHOOK_VERSION\s*=\s*\"v[^\"]*\"', f'WINKEYHOOK_VERSION = \"v{sys.argv[1]}\"', c); open(f, 'w', encoding='utf-8').write(c)" %APP_VERSION%
python -c "import re, sys; f='setup_script.iss'; c=open(f, encoding='utf-8').read(); c=re.sub(r'(?m)^AppVersion=.*', f'AppVersion={sys.argv[1]}', c); open(f, 'w', encoding='utf-8').write(c)" %APP_VERSION%

echo.
echo [2/3] Compilation C++ avec MSVC...
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul 2>nul
)
if %ERRORLEVEL% NEQ 0 (
    call "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul 2>nul
)

cd ..\src
cl.exe /nologo /EHsc "WinKeyHook.cpp" /link user32.lib advapi32.lib winhttp.lib urlmon.lib shell32.lib /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERREUR] La compilation C++ a echoue.
    set /p DUMMY="Appuyez sur Entree pour quitter..."
    exit /b %ERRORLEVEL%
)

if exist "WinKeyHook.obj" del "WinKeyHook.obj"

echo.
echo [3/3] Generation de l'installateur (Inno Setup)...
cd ..\build
set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo [ERREUR] Inno Setup compiler introuvable a !ISCC!
    set /p DUMMY="Appuyez sur Entree pour quitter..."
    exit /b 1
)

"%ISCC%" /Q "setup_script.iss"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERREUR] La generation Inno Setup a echoue.
    set /p DUMMY="Appuyez sur Entree pour quitter..."
    exit /b %ERRORLEVEL%
)

move /Y "WinKeyHook_setup.exe" "%RELEASES_DIR%\%APP_NAME%_v%APP_VERSION%_Setup.exe" >nul

echo.
echo ======================================================
echo    BUILD TERMINE AVEC SUCCES !
echo ======================================================
echo.
echo   Executable local  : src\WinKeyHook.exe
echo   Installateur      : releases\%APP_NAME%_v%APP_VERSION%_Setup.exe
echo.
set /p DUMMY="Appuyez sur Entree pour quitter..."
