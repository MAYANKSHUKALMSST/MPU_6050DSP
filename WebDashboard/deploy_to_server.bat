@echo off
:: ============================================================
:: PredictivEdge — Deploy dashboard updates to Oracle server
:: Uses pscp.exe (ships with PuTTY) + your existing .ppk key
:: ============================================================
setlocal

set KEY=C:\Users\mayan\Downloads\pvt_keys.ppk
set HOST=ubuntu@161.118.167.196
set REMOTE=/opt/predictivEdge
set PSCP=pscp
set PLINK=plink

echo.
echo =============================================
echo  PredictivEdge Server Deployment
echo  Target: %HOST%
echo =============================================
echo.

:: Check pscp is available
where pscp >nul 2>&1
if errorlevel 1 (
    echo [ERROR] pscp.exe not found in PATH.
    echo Install PuTTY from https://www.putty.org/ or add it to PATH.
    echo Common location: C:\Program Files\PuTTY\pscp.exe
    pause
    exit /b 1
)

echo [1/4] Uploading server.js...
pscp -i "%KEY%" -batch server.js %HOST%:%REMOTE%/server.js
if errorlevel 1 goto :error

echo [2/4] Uploading index.html...
pscp -i "%KEY%" -batch public\index.html %HOST%:%REMOTE%/public/index.html
if errorlevel 1 goto :error

echo [3/4] Uploading script.js...
pscp -i "%KEY%" -batch public\script.js %HOST%:%REMOTE%/public/script.js
if errorlevel 1 goto :error

echo [4/4] Uploading style.css...
pscp -i "%KEY%" -batch public\style.css %HOST%:%REMOTE%/public/style.css
if errorlevel 1 goto :error

echo.
echo [5/5] Restarting PM2 service...
plink -i "%KEY%" -batch %HOST% "pm2 restart predictivEdge && pm2 list"
if errorlevel 1 (
    echo [WARN] PM2 restart may have failed — check server manually.
)

echo.
echo =============================================
echo  Deployment complete!
echo  Dashboard: http://161.118.167.196:3000
echo =============================================
echo.
pause
exit /b 0

:error
echo.
echo [ERROR] Upload failed. Check key path and server connectivity.
pause
exit /b 1
