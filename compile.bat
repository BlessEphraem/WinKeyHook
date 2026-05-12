@echo off
setlocal enabledelayedexpansion

:: Attempting to load the Visual Studio environment WITHOUT hiding the text (without >nul)
echo [INFO] Attempting to load the Visual Studio environment...
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64

:: Checking if cl.exe is actually there
where cl.exe >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] cl.exe is still not found.
    echo Look at the text just above: does it say "The system cannot find the path specified"?
    echo Check if the "18" folder shouldn't rather be "2022" or "2019".
    pause
    goto :EOF
)

:: 2. Displaying the menu
cls
echo ====================================================
echo        Interactive C++ Compiler (MSVC)
echo ====================================================
echo.
set count=0

for %%f in (*.cpp) do (
    set /a count+=1
    set "file[!count!]=%%f"
    echo [!count!] %%f
)

if %count%==0 (
    echo No .cpp files found in the current directory.
    pause
    goto :EOF
)

echo.
set /p choix="Enter the number of the file to compile (1-%count%): "

if not defined file[%choix%] (
    echo Invalid choice. Restart the script.
    pause
    goto :EOF
)

set "cible=!file[%choix%]!"
echo.
echo Compiling "!cible!"...
echo ----------------------------------------------------

:: 3. Compilation with cl.exe and user32.lib
cl.exe /nologo /EHsc "!cible!" /link user32.lib advapi32.lib

:: 4. Cleaning up the .obj file
if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] Compilation completed successfully!
    
    for %%I in ("!cible!") do set "nom_base=%%~nI"
    
    if exist "!nom_base!.obj" (
        del "!nom_base!.obj"
        echo [INFO] Cleanup: "!nom_base!.obj" deleted.
    )
) else (
    echo.
    echo [ERROR] Compilation failed. Check the errors above.
)

echo ====================================================
pause