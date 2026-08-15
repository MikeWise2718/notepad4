@echo off
setlocal

:: Installs Notepad4.ini to the locations Notepad4 reads it from.
::
:: Notepad4 prefers an ini next to the executable (portable mode) and falls
:: back to %LOCALAPPDATA%\Notepad4. Both are installed here.
::
:: NOTE: these are live settings files. Notepad4 rewrites them on exit with
:: window position, zoom level, recent files and so on. Overwriting one throws
:: that away, so any existing file is backed up first.

echo Installing Notepad4 configuration...
echo.

set "SOURCE=%~dp0Notepad4.ini"
if not exist "%SOURCE%" (
    echo ERROR: %SOURCE% not found.
    exit /b 1
)

:: The portable install directory: where Notepad4.exe lives.
::   install-config.bat            install to the defaults below
::   install-config.bat E:\tools   install to E:\tools instead of D:\ut
::   install-config.bat /n         dry run: show what would happen, change nothing
set "UT_DIR=D:\ut"
set "DRYRUN="
if /I "%~1"=="/n" (
    set "DRYRUN=1"
    echo *** DRY RUN - no files will be changed ***
    echo.
) else (
    if not "%~1"=="" set "UT_DIR=%~1"
)

call :install "%LOCALAPPDATA%\Notepad4"
call :install "%UT_DIR%"

echo.
echo Installation complete.
endlocal
exit /b 0

:install
set "TARGET=%~1"
if not exist "%TARGET%" (
    echo Creating %TARGET%
    if not defined DRYRUN mkdir "%TARGET%"
)
if exist "%TARGET%\Notepad4.ini" (
    echo Backing up existing %TARGET%\Notepad4.ini to Notepad4.ini.bak
    if not defined DRYRUN copy /Y "%TARGET%\Notepad4.ini" "%TARGET%\Notepad4.ini.bak" >nul
)
echo Copying Notepad4.ini to %TARGET%
if not defined DRYRUN copy /Y "%SOURCE%" "%TARGET%\Notepad4.ini" >nul
exit /b 0
