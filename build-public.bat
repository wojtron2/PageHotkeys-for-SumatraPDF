@echo off
setlocal EnableExtensions EnableDelayedExpansion

pushd "%~dp0" || exit /b 1

echo Stopping running PageHotkeys.exe, if any...
taskkill /IM PageHotkeys.exe /F >nul 2>nul
taskkill /IM PdfController.exe /F >nul 2>nul

set "ROOT=%CD%"
set "OUTDIR=%ROOT%\build_public"
set "OUTEXE=%OUTDIR%\PageHotkeys.exe"
set "RESRC=%ROOT%\PageHotkeys.rc"
set "RESFILE=%OUTDIR%\PageHotkeys.res"
set "OBJFILE=%OUTDIR%\main.obj"

if /I not "%OUTDIR%"=="%ROOT%\build_public" (
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

if exist "%OBJFILE%" (
    del /f /q "%OBJFILE%"
)

call :LoadMsvcEnvironment
if not "%ERRORLEVEL%"=="0" (
    popd
    exit /b 1
)

where rc.exe >nul 2>nul
if not "%ERRORLEVEL%"=="0" (
    echo rc.exe was not found after loading the MSVC environment.
    popd
    exit /b 1
)

echo Building resources with rc.exe...
rc.exe /nologo /fo "%RESFILE%" "%RESRC%"
if not "%ERRORLEVEL%"=="0" (
    popd
    exit /b %ERRORLEVEL%
)

echo Building public AV-friendly PageHotkeys.exe with MSVC...
cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MD /GS /guard:cf /DUNICODE /D_UNICODE /DPAGEHOTKEYS_PUBLIC_BUILD /DPAGEHOTKEYS_SUMATRA_CLI_DDE /Fo"%OBJFILE%" src\main.cpp "%RESFILE%" /Fe:"%OUTEXE%" user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /GUARD:CF /OPT:REF /OPT:ICF
set "BUILD_ERROR=%ERRORLEVEL%"

popd
exit /b %BUILD_ERROR%

:TryVsDevCmd
if not exist "%~1" exit /b 1
call "%~1" -no_logo -arch=x64 -host_arch=x64
where cl.exe >nul 2>nul
exit /b %ERRORLEVEL%

:LoadMsvcEnvironment
where cl.exe >nul 2>nul
if "%ERRORLEVEL%"=="0" exit /b 0

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    set "VSINSTALL="
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
    if not "!VSINSTALL!"=="" (
        call :TryVsDevCmd "!VSINSTALL!\Common7\Tools\VsDevCmd.bat"
        if not errorlevel 1 exit /b 0
    )
)

call :TryVsDevCmd "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if "%ERRORLEVEL%"=="0" exit /b 0
call :TryVsDevCmd "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if "%ERRORLEVEL%"=="0" exit /b 0
call :TryVsDevCmd "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"
if "%ERRORLEVEL%"=="0" exit /b 0
call :TryVsDevCmd "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
if "%ERRORLEVEL%"=="0" exit /b 0
call :TryVsDevCmd "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat"
if "%ERRORLEVEL%"=="0" exit /b 0
call :TryVsDevCmd "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat"
if "%ERRORLEVEL%"=="0" exit /b 0
call :TryVsDevCmd "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\Common7\Tools\VsDevCmd.bat"
if "%ERRORLEVEL%"=="0" exit /b 0
call :TryVsDevCmd "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\Common7\Tools\VsDevCmd.bat"
if "%ERRORLEVEL%"=="0" exit /b 0

echo MSVC cl.exe was not found.
echo Install Visual Studio 2022 Build Tools with "Desktop development with C++".
exit /b 1
