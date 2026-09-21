@echo off
REM ============================================================================
REM  hid_firmware_flash.bat
REM  Compile PUIS televerse le firmware S3-KBD sur la carte.
REM
REM  Carte : ESP32-S3 SuperMini "4 Mo" (N4R2) = 4 Mo flash + PSRAM QSPI.
REM  -> FlashSize=4M (le README documente une N8R2 8 Mo : ce script vise le 4 Mo).
REM  USBMode=default = "USB-OTG (TinyUSB)" : INDISPENSABLE au HID.
REM  PartitionScheme=huge_app + PSRAM=enabled (BLE + Wi-Fi + USB + app embarquee).
REM
REM  Usage :  hid_firmware_flash.bat [COMx]
REM    - sans argument : auto-detection du port ESP32, sinon COM7 par defaut.
REM    - avec argument : force le port (ex. hid_firmware_flash.bat COM5).
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
title Flash firmware S3-KBD

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

REM --- Port : 1) argument  2) auto-detection ESP32  3) COM7 par defaut ---------
set "PORT=%~1"
if not defined PORT call :detect_port
if not defined PORT set "PORT=COM7"

echo ============================================================
echo   S3-KBD - Flash firmware (SuperMini 4 Mo + PSRAM)
echo   arduino-cli : %ARDUINO_CLI%
echo   Port cible  : %PORT%   (forcer : "%~nx0" COMx)
echo ============================================================

echo [1/3] Generation de web_assets.h ...
call :regen

echo [2/3] Compilation ...
"%ARDUINO_CLI%" compile --fqbn %FQBN% "%SKETCH%"
if errorlevel 1 goto :err_build

echo [3/3] Televersement sur %PORT% ...
"%ARDUINO_CLI%" upload --fqbn %FQBN% -p %PORT% "%SKETCH%"
if errorlevel 1 goto :err_upload

echo.
echo === Upload OK - APPUYEZ SUR RESET pour demarrer le firmware ===
echo     (sinon la carte peut rester en ROM "USB JTAG/serial debug unit")
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

:detect_port
REM Auto-detection via "arduino-cli board list".
REM 1) priorite a une carte identifiee ESP32 ; 2) sinon 1er port COM liste.
REM (un autre peripherique peut occuper un COM plus bas : ne pas prendre "le 1er".)
set "TMPLIST=%TEMP%\s3kbd_ports.txt"
"%ARDUINO_CLI%" board list > "%TMPLIST%" 2>nul
for /f "tokens=1" %%P in ('findstr /b /i /c:"COM" "%TMPLIST%" ^| findstr /i "esp32"') do (
  if not defined PORT set "PORT=%%P"
)
if not defined PORT for /f "tokens=1" %%P in ('findstr /b /i /c:"COM" "%TMPLIST%"') do (
  if not defined PORT set "PORT=%%P"
)
del "%TMPLIST%" >nul 2>&1
exit /b 0

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

:err_upload
echo [X] Echec du televersement sur %PORT%.
echo     Verifier le port COM (Gestionnaire de peripheriques) et, au besoin,
echo     le mode boot : maintenir BOOT, appuyer/relacher RESET, relacher BOOT.
set "RC=1"
goto :end

:end
if defined DBLCLICK pause
endlocal & exit /b %RC%
