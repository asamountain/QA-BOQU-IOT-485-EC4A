@echo off
REM ============================================================================
REM BOQU IOT-485-EC4A Automated Test Runner
REM One-Click Solution for QA Intern Testing
REM ============================================================================
REM FEATURES:
REM   - Auto-elevates to Administrator (triggers UAC prompt automatically)
REM   - Auto-detects USB-SERIAL CH340 device via usbipd
REM   - Binds and attaches device to WSL2
REM   - Compiles and launches smart_logger in WSL
REM ============================================================================

setlocal enabledelayedexpansion

REM ============================================================================
REM STEP 0: SELF-ELEVATION HACK (Auto-Admin)
REM ============================================================================
REM Check if we already have admin privileges using "net session"
REM If not, re-launch this script as Administrator via PowerShell

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo.
    echo ============================================================================
    echo   Requesting Administrator Privileges...
    echo ============================================================================
    echo.
    echo This script requires Administrator access for USB passthrough.
    echo A UAC prompt will appear - please click "Yes" to continue.
    echo.
    
    REM Use PowerShell to re-launch this batch file with elevation
    REM -Verb RunAs triggers the UAC elevation prompt
    REM %~dpnx0 expands to the full path of this batch file
    REM We use Start-Process with -Wait so the elevated window stays open
    
    powershell -Command "Start-Process -FilePath '%~dpnx0' -Verb RunAs -Wait"
    
    REM Exit the non-elevated instance
    exit /b 0
)

REM If we reach here, we have admin privileges
echo.
echo ============================================================================
echo   BOQU Sensor Auto-Test Launcher [ADMINISTRATOR]
echo ============================================================================
echo.

echo [1/5] Checking for usbipd installation...
where usbipd >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] usbipd-win is not installed!
    echo Please install it first: winget install usbipd
    echo.
    pause
    exit /b 1
)
echo      OK - usbipd found

echo.
echo [2/5] Scanning for USB-SERIAL CH340 device...

REM ============================================================================
REM SIMPLIFIED BUS ID DETECTION USING LENGTH CHECK
REM ============================================================================
REM Valid Bus ID: Short format (e.g., 6-1, 1-2, 10-3) - Usually < 10 chars
REM Invalid UUID: Long format (e.g., c465442d-54e8-...) - Usually > 30 chars
REM Strategy: Accept only IDs shorter than 10 characters

set BUSID=
set DEVICE_FOUND=0

for /f "tokens=1,* delims= " %%a in ('usbipd list ^| findstr /i "CH340"') do (
    REM %%a contains the first token (potential Bus ID)
    set TEMP_ID=%%a
    
    REM Simple length check using string manipulation
    REM Extract 10 characters - if the result is empty, the string is < 10 chars
    set ID_TEST=!TEMP_ID:~9,1!
    
    REM If position 9 (10th character) doesn't exist, the ID is < 10 chars (valid)
    if "!ID_TEST!"=="" (
        set BUSID=!TEMP_ID!
        set DEVICE_FOUND=1
        echo      Found Valid Bus ID: %%a %%b
        echo      Selected BUSID: !TEMP_ID!
        REM Jump immediately to device state check
        goto :device_found
    ) else (
        echo      Skipped UUID: %%a %%b [Too long]
    )
)

REM Only reach here if NO valid device was found
echo [ERROR] USB-SERIAL CH340 not found with valid Bus ID!
echo.
echo Please check:
echo   1. USB adapter is plugged in
echo   2. Device appears in Device Manager
echo   3. Device has a standard Bus ID (not UUID)
echo.
echo Full device list:
usbipd list
echo.
pause
exit /b 1

:device_found
REM Valid device found - continue to next step

echo.
echo [3/5] Preparing device for WSL...

REM Check if device is already attached (if so, skip bind and attach steps)
usbipd list | findstr /i "!BUSID!" | findstr /i "Attached" >nul 2>&1
if %errorLevel% equ 0 (
    echo      Device already attached to WSL - Ready!
    goto :run_program
)

REM ============================================================================
REM FORCE BIND - Always bind regardless of current state
REM ============================================================================
REM This ensures the device is shared even if state detection fails
REM The --force flag handles cases where it's already bound

echo      Binding device to allow WSL access...
usbipd bind --busid !BUSID! --force >nul 2>&1

REM Check if bind succeeded (errorlevel 0 or 1 are both acceptable)
REM Error code 1 often means "already bound" which is fine
if %errorLevel% LEQ 1 (
    echo      Device successfully bound/shared
) else (
    echo [WARNING] Bind command returned error code %errorLevel%
    echo           Will attempt to attach anyway...
)

REM Small delay to let the bind take effect
timeout /t 1 /nobreak >nul

echo.
echo [4/5] Attaching device to WSL...

REM Ensure WSL is running before attaching
echo      Waking up WSL...
wsl -e echo "WSL Ready" >nul 2>&1
timeout /t 1 /nobreak >nul

REM Use START /MIN for non-blocking attach (prevents script from hanging)
REM The /B flag runs without creating a new window, /MIN minimizes if window is created
echo      Starting USB attach (non-blocking)...
start "USB_Link" /MIN /B usbipd attach --wsl --busid !BUSID!

REM Give the attach command time to complete
echo      Waiting for USB link to establish...
timeout /t 4 /nobreak >nul

REM Verify attachment status
usbipd list | findstr /i "!BUSID!" | findstr /i "Attached" >nul 2>&1
if %errorLevel% neq 0 (
    echo [WARNING] Device may not be fully attached yet.
    echo           Will attempt to run anyway...
    timeout /t 2 /nobreak >nul
) else (
    echo      Device successfully attached!
)

REM Wait for device to initialize in WSL (serial port needs time to appear)
echo      Waiting for serial port to initialize...
timeout /t 3 /nobreak >nul

:run_program
echo.
echo [5/5] Launching Smart Logger in WSL...
echo ============================================================================
echo.
echo NOTE: If prompted for password, enter your WSL sudo password.
echo       Press Ctrl+C to stop logging.
echo.
echo ============================================================================
echo  If the program is missing or outdated, compile manually in WSL:
echo.
echo    g++ smart_logger.cpp -o smart_logger -I/usr/include/modbus -lmodbus
echo.
echo ============================================================================
echo.

REM ============================================================================
REM DYNAMIC PATH DETECTION
REM ============================================================================
REM %~dp0 = Directory of this batch file (e.g., C:\Users\iocrops admin\Coding\QA-BOQU-IOT-485-EC4A\)
REM Remove trailing backslash: %~dp0 ends with \, so we strip it
set "WIN_PROJECT_PATH=%~dp0"
set "WIN_PROJECT_PATH=%WIN_PROJECT_PATH:~0,-1%"

REM Convert Windows path to WSL path using wslpath
REM Example: C:\Users\iocrops admin\Coding\QA -> /mnt/c/Users/iocrops admin/Coding/QA
echo      Detecting project path...
for /f "delims=" %%i in ('wsl wslpath -a "%WIN_PROJECT_PATH%"') do set "WSL_PROJECT_PATH=%%i"

echo      Windows Path: %WIN_PROJECT_PATH%
echo      WSL Path: %WSL_PROJECT_PATH%
echo.

REM Execute smart_logger using the dynamically detected path
REM The path is quoted to handle spaces in usernames/folders
wsl bash -c "cd \"%WSL_PROJECT_PATH%\" && sudo ./smart_logger"

REM After program exits
echo.
echo ============================================================================
echo   Test Session Ended
echo ============================================================================
echo.
echo Data saved to: ec_data_log.csv
echo.
echo Next steps:
echo   1. Run visualization: python3 plot_data.py
echo   2. Or restart test: run_test.bat
echo.

REM Ask if user wants to unbind the device
set /p UNBIND="UNBIND USB device from WSL? (Y/N): "
if /i "!UNBIND!"=="Y" (
    echo UNBINDing all devices...
    usbipd unbind --all
    echo All devices unbinded. Available for Windows use.
)

echo.
pause
