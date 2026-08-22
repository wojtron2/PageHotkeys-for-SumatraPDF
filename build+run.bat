@echo off
setlocal

pushd "%~dp0" || exit /b 1

call ".\build.bat"
if not "%ERRORLEVEL%"=="0" (
    set "BUILD_ERROR=%ERRORLEVEL%"
    popd
    exit /b %BUILD_ERROR%
)

start "" ".\build\PageHotkeys.exe"

popd
exit /b 0
