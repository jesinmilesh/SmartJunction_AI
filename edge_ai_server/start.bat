@echo off
REM SmartJunction AI — Node.js Quick Start (Windows)
REM Run this from the edge_ai_server folder

echo ============================================================
echo  SmartJunction AI — Node.js Edge Server v2.0
echo ============================================================

REM Check Node.js
where node >nul 2>&1
if errorlevel 1 (
    if exist "C:\Program Files\nodejs\node.exe" (
        SET "PATH=%PATH%;C:\Program Files\nodejs"
        echo [OK] Found Node.js in C:\Program Files\nodejs
    ) else (
        echo [ERROR] Node.js not found in PATH or C:\Program Files\nodejs.
        echo Install from https://nodejs.org (v18+)
        pause & exit /b 1
    )
)

REM Install dependencies
if not exist "node_modules" (
    echo [1/3] Installing npm dependencies...
    npm install
    if errorlevel 1 ( echo [ERROR] npm install failed & pause & exit /b 1 )
)

REM Copy .env if missing
if not exist ".env" (
    echo [2/3] Creating .env from template...
    copy .env.example .env
    echo [!] Edit .env to set your camera IPs before continuing!
)

REM Download YOLO model if missing
if not exist "yolov8n.onnx" (
    echo [3/3] Downloading YOLOv8n ONNX model...
    node download-model.js
) else (
    echo [3/3] YOLO model already present.
)

echo.
echo  Dashboard  ^>^>  http://localhost:5000
echo  MQTT       ^>^>  localhost:1883  ^(start Mosquitto separately^)
echo ============================================================
echo.

node server.js
pause
