@echo off
REM ============================================================================
REM  hid_firmware_compile.bat
REM  Compile le firmware S3-KBD sans televerser.
REM
REM  Carte : ESP32-S3 SuperMini "4 Mo" (N4R2) = 4 Mo flash + PSRAM QSPI.
REM  -> FlashSize=4M (le README documente une N8R2 8 Mo : ce script vise le 4 Mo).
REM  USBMode=default = "USB-OTG (TinyUSB)" : INDISPENSABLE au HID.
REM  PartitionScheme=huge_app : l'app par defaut (~1,3 Mo) ne suffit plus
REM  (BLE + Wi-Fi + USB + app embarquee). PSRAM=enabled : allocs Wi-Fi/LWIP.
REM
REM  Regenere web_assets.h (app embarquee) puis compile.
REM  Usage :  hid_firmware_compile.bat
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
title Compil firmware S3-KBD

REM --- Pause finale seulement si lance par double-clic (fenetre Explorer) ------
set "DBLCLICK="
echo %cmdcmdline% | find /i "%~0" >nul 2>&1 && set "DBLCLICK=1"

REM --- Dossiers (ce .bat est dans S3\) -----------------------------------------
set "S3_DIR=%~dp0"
set "SKETCH=%S3_DIR%hid_firmware"
set "GEN=%SKETCH%\tools\gen_web_assets.py"

REM --- FQBN : carte 4 Mo + huge_app + PSRAM + USB-OTG (HID) --------------------
set "FQBN=esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled"

call :find_cli
if errorlevel 1 goto :err_cli

echo ============================================================
echo   S3-KBD - Compilation (SuperMini 4 Mo + PSRAM)
echo   arduino-cli : %ARDUINO_CLI%
echo ============================================================

echo [1/2] Generation de web_assets.h ...
call :regen

echo [2/2] Compilation ...
"%ARDUINO_CLI%" compile --fqbn %FQBN% "%SKETCH%"
if errorlevel 1 goto :err_build

echo.
echo === Compilation OK ===
echo     Pour televerser : hid_firmware_flash.bat [COMx]
set "RC=0"
goto :end

REM ============================ sous-programmes ===============================
:find_cli
REM Localise arduino-cli : variable ARDUINO_CLI, puis PATH, puis Arduino IDE.
if defined ARDUINO_CLI if exist "%ARDUINO_CLI%" exit /b 0
for %%I in (arduino-cli.exe) do if not "%%~$PATH:I"=="" set "ARDUINO_CLI=%%~$PATH:I"
if defined ARDUINO_CLI exit /b 0
set "ARDUINO_CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if exist "%ARDUINO_CLI%" exit /b 0
set "ARDUINO_CLI="
exit /b 1

:regen
REM Regenere l'app embarquee ; non bloquant si Python absent (assets existants).
set "PY="
where python >nul 2>&1 && set "PY=python"
if not defined PY (
  where py >nul 2>&1 && set "PY=py -3"
)
if not defined PY (
  echo     [!] Python introuvable dans le PATH - web_assets.h existant conserve.
  exit /b 0
)
%PY% "%GEN%"
if errorlevel 1 echo     [!] Echec de la generation - web_assets.h existant conserve.
exit /b 0

REM =============================== erreurs ====================================
:err_cli
echo [X] arduino-cli introuvable.
echo     Attendu : "%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
echo     Sinon : definir la variable ARDUINO_CLI ou l'ajouter au PATH.
set "RC=1"
goto :end

:err_build
echo [X] Echec de la compilation (voir messages ci-dessus).
set "RC=1"
goto :end

:end
if defined DBLCLICK pause
endlocal & exit /b %RC%
