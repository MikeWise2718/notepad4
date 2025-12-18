@echo off
setlocal

echo Installing Notepad4 configuration...

:: Install to %LOCALAPPDATA%\Notepad4
set "TARGET_DIR=%LOCALAPPDATA%\Notepad4"
if not exist "%TARGET_DIR%" (
    echo Creating %TARGET_DIR%
    mkdir "%TARGET_DIR%"
)
echo Copying Notepad4.ini to %TARGET_DIR%
copy /Y "%~dp0Notepad4.ini" "%TARGET_DIR%\Notepad4.ini"

:: Install to C:\ut
set "UT_DIR=C:\ut"
if not exist "%UT_DIR%" (
    echo Creating %UT_DIR%
    mkdir "%UT_DIR%"
)
echo Copying Notepad4.ini to %UT_DIR%
copy /Y "%~dp0Notepad4.ini" "%UT_DIR%\Notepad4.ini"

echo.
echo Installation complete.
endlocal
