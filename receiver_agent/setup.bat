@echo off
REM ════════════════════════════════════════════════════════════════════
REM  GREENPOWER RECEIVER AGENT — ONE-TIME SETUP
REM
REM  Double-click this file once. It will:
REM    1. Install dependencies (npm install)
REM    2. Ask for your website URL + API key, if not already configured
REM    3. Register the agent to start automatically at login, hidden
REM       (no console window, logs to agent.log instead)
REM    4. Start it right now too, so you don't need to log out/in first
REM
REM  After this, using it is just: plug in the receiver, click the
REM  notification that pops up. Nothing else to run, ever, unless the
REM  URL/API key change (re-run this script, or edit config.json by hand).
REM ════════════════════════════════════════════════════════════════════

cd /d "%~dp0"

echo Greenpower Receiver Agent - Setup
echo ==================================
echo.

where node >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Node.js is not installed or not on PATH.
    echo         Install it from https://nodejs.org ^(LTS version^), then re-run this script.
    pause
    exit /b 1
)

echo Installing dependencies...
call npm install
if errorlevel 1 (
    echo [ERROR] npm install failed - see above. Fix that, then re-run this script.
    pause
    exit /b 1
)

if exist config.json goto :configdone

echo.
echo No config.json found yet - let's set it up.
echo ^(Same values as: Railway TELEMETRY_API_KEY, and your dashboard URL^)
echo.
set /p WEBSITE_URL="Website URL, e.g. https://telemetry.yourdomain.com/api/telemetry: "
set /p API_KEY="API key: "

> config.json echo {
>> config.json echo   "websiteUrl": "%WEBSITE_URL%",
>> config.json echo   "apiKey": "%API_KEY%"
>> config.json echo }

echo Saved config.json
goto :afterconfig

:configdone
echo config.json already exists - leaving it as-is.
echo ^(Delete it and re-run this script if you need to change the URL/key.^)

:afterconfig
echo.
echo Setting up auto-start at login...

set STARTUP_DIR=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
set LAUNCHER=%STARTUP_DIR%\GreenpowerReceiverAgent.vbs

> "%LAUNCHER%" (
    echo Set WshShell = CreateObject^("WScript.Shell"^)
    echo WshShell.CurrentDirectory = "%~dp0"
    echo WshShell.Run "node agent.js", 0, False
)

echo Auto-start configured. The agent will now start silently every time you log in.
echo.

if not exist agent.pid goto :nooldprocess

echo Stopping previous agent instance...
set /p OLDPID=<agent.pid
taskkill /F /PID %OLDPID% >nul 2>nul
del /f /q agent.pid >nul 2>nul
REM Give Windows a moment to actually release any serial port the old
REM process was holding open before the new instance tries to use it.
timeout /t 2 /nobreak >nul

:nooldprocess
echo Starting the agent now...
wscript.exe "%LAUNCHER%"

echo.
echo ==================================
echo Done. From now on: just plug in the receiver and click the
echo notification that appears. Check agent.log in this folder if
echo something doesn't seem to be working.
echo.
pause
