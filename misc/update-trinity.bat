@echo off
REM Downloads the latest Trinity mod pk3s from GitHub and removes
REM any stale checksum-named variants left by auto-download.

set "REPO=ernie/trinity"
set "BASE_URL=https://github.com/%REPO%/releases/latest/download"

set "SCRIPT_DIR=%~dp0"
set "BASEQ3=%SCRIPT_DIR%baseq3"
set "MISSIONPACK=%SCRIPT_DIR%missionpack"

REM Remove checksummed variants (e.g., pak8t.0a1b2c3d.pk3)
REM and the current copy, then download fresh.
if exist "%BASEQ3%" (
    del /q "%BASEQ3%\pak8t.*.pk3" 2>nul
    del /q "%BASEQ3%\pak8t.pk3" 2>nul
    del /q "%BASEQ3%\pak3t.*.pk3" 2>nul
    del /q "%BASEQ3%\pak3t.pk3" 2>nul
    del /q "%BASEQ3%\zzz-trinity-announcer.*.pk3" 2>nul
    del /q "%BASEQ3%\zzz-trinity-announcer.pk3" 2>nul
)
if exist "%MISSIONPACK%" (
    del /q "%MISSIONPACK%\pak8t.*.pk3" 2>nul
    del /q "%MISSIONPACK%\pak8t.pk3" 2>nul
    del /q "%MISSIONPACK%\pak3t.*.pk3" 2>nul
    del /q "%MISSIONPACK%\pak3t.pk3" 2>nul
)

echo Downloading latest Trinity pk3s...
curl -fL -o "%BASEQ3%\pak8t.pk3" "%BASE_URL%/pak8t.pk3" && echo   baseq3\pak8t.pk3 OK || echo   baseq3\pak8t.pk3 FAILED
curl -fL -o "%MISSIONPACK%\pak3t.pk3" "%BASE_URL%/pak3t.pk3" && echo   missionpack\pak3t.pk3 OK || echo   missionpack\pak3t.pk3 FAILED
curl -fL -o "%BASEQ3%\zzz-trinity-announcer.pk3" "%BASE_URL%/zzz-trinity-announcer.pk3" && echo   baseq3\zzz-trinity-announcer.pk3 OK || echo   baseq3\zzz-trinity-announcer.pk3 FAILED

echo Done.
pause
