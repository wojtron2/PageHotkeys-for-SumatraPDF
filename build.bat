@echo off
setlocal

pushd "%~dp0" || exit /b 1

echo Stopping running PageHotkeys.exe, if any...
taskkill /IM PageHotkeys.exe /F >nul 2>nul
taskkill /IM PdfController.exe /F >nul 2>nul

set "ROOT=%CD%"
set "OUTDIR=%ROOT%\build"
set "OUTEXE=%OUTDIR%\PageHotkeys.exe"
set "LEGACY_EXE=%OUTDIR%\PdfController.exe"
set "RESRC=%ROOT%\PageHotkeys.rc"
set "RESOBJ=%OUTDIR%\PageHotkeys.res.o"
set "RESFILE=%OUTDIR%\PageHotkeys.res"

if /I not "%OUTDIR%"=="%ROOT%\build" (
    echo Refusing to delete unexpected build path: "%OUTDIR%"
    popd
    exit /b 1
)

if not exist "%OUTDIR%" (
    mkdir "%OUTDIR%"
    if not exist "%OUTDIR%" (
        echo Could not create "%OUTDIR%".
        popd
        exit /b 1
    )
)

if exist "%OUTEXE%" (
    echo Removing "%OUTEXE%"...
    del /f /q "%OUTEXE%"
    if exist "%OUTEXE%" (
        echo Could not remove "%OUTEXE%".
        popd
        exit /b 1
    )
)

if exist "%LEGACY_EXE%" (
    echo Removing "%LEGACY_EXE%"...
    del /f /q "%LEGACY_EXE%"
    if exist "%LEGACY_EXE%" (
        echo Could not remove "%LEGACY_EXE%".
        popd
        exit /b 1
    )
)

where cl.exe >nul 2>nul
if %ERRORLEVEL%==0 (
    echo Building with MSVC cl.exe...
    rc.exe /nologo /fo "%RESFILE%" "%RESRC%"
    if not "%ERRORLEVEL%"=="0" (
        popd
        exit /b %ERRORLEVEL%
    )
    cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /DUNICODE /D_UNICODE src\main.cpp "%RESFILE%" /Fe:%OUTEXE% user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib /link /SUBSYSTEM:WINDOWS
    popd
    exit /b %ERRORLEVEL%
)

where gcc.exe >nul 2>nul
if %ERRORLEVEL%==0 (
    echo Building with MinGW gcc C++ front-end...
    windres.exe -i "%RESRC%" -o "%RESOBJ%"
    if not "%ERRORLEVEL%"=="0" (
        popd
        exit /b %ERRORLEVEL%
    )
    gcc.exe -x c++ -std=c++20 -O2 -s -Wall -Wextra -Wpedantic -municode -mwindows -DUNICODE -D_UNICODE src\main.cpp -x none "%RESOBJ%" -o "%OUTEXE%" -static -static-libgcc -lshell32 -lcomdlg32 -lcomctl32 -lgdi32 -luser32
    popd
    exit /b %ERRORLEVEL%
)

echo No supported compiler found in PATH.
echo Install Visual Studio Build Tools or make MinGW gcc available in PATH.
popd
exit /b 1
