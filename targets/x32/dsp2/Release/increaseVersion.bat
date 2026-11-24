@echo off
setlocal enabledelayedexpansion

set "infile=%1"
set "outfile=%2"

:: Step 1 - read first line of header-file and search for #define DSP_VERSION
for /f "tokens=1,2,3" %%a in (%infile%) do (
    if "%%a"=="#define" if "%%b"=="DSP_VERSION" (
        set "current_version=%%c"
    )
)

:: Step 2  - divide point-release-number to main and revision
for /f "tokens=1,2 delims=." %%i in ("%current_version%") do (
    set "major=%%i"
    set "minor=%%j"
)

:: Step 3 - get leading zero and increase by 1 (otherwise batch would take this as octal)
if "%minor:~0,1%"=="0" (
    set /a "minor_num=1%minor% - 100"
) else (
    set "minor_num=%minor%"
)

set /a "minor_num+=1"

:: Step 4 - add leading zero if minor is below 10
if %minor_num% lss 10 (
    set "new_minor=0%minor_num%"
) else (
    set "new_minor=%minor_num%"
)

:: if revision is 100 increase major-version
if %new_minor%==100 (
    set /a "major+=1"
    set "new_minor=00"
)

set "new_version=%major%.%new_minor%"

:: Step 5 - write new header
echo #define DSP_VERSION %new_version% > %outfile%
move /y %outfile% %infile% > nul

echo Increased version to %new_version% .
endlocal