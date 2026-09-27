@echo off
REM ============================================================================
REM  hid_firmware_compile.bat
REM  Compile le firmware S3-KBD (sans televerser) et publie les binaires
REM  dans le sous-dossier firmware\hid_firmware_vYYMM.dd\ :
REM    hid_firmware_vYYMM.dd.bin              application          (0x10000)
REM    hid_firmware_vYYMM.dd.bootloader.bin   bootloader           (0x0)
REM    hid_firmware_vYYMM.dd.partitions.bin   table de partitions  (0x8000)
REM  Les trois fichiers vont ensemble : hid_firmware_flash.bat les ecrit aux
REM  bons offsets sans toucher a la NVS (reglages / appairages conserves).
REM  Une 2e compilation le meme jour remplace la version du jour.
REM
REM  Carte : ESP32-S3 SuperMini "4 Mo" (N4R2) = 4 Mo flash + PSRAM QSPI.
REM  -> FlashSize=4M (le README documente une N8R2 8 Mo : ce script vise le 4 Mo).
REM  USBMode=default = "USB-OTG (TinyUSB)" : INDISPENSABLE au HID.
REM  PartitionScheme=huge_app : l'app par defaut (~1,3 Mo) ne suffit plus
REM  (BLE + Wi-Fi + USB + app embarquee). PSRAM=enabled : allocs Wi-Fi/LWIP.
REM
REM  Regenere web_assets.h (site captif embarque, depuis WEB\Keyboard) puis
REM  compile. Regeneration BLOQUANTE : pas de firmware avec un site perime.
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
set "BUILD_DIR=%S3_DIR%build"
set "FW_DIR=%S3_DIR%firmware"

REM --- FQBN : carte 4 Mo + huge_app + PSRAM + USB-OTG (HID) --------------------
set "FQBN=esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled"

call :find_cli
if errorlevel 1 goto :err_cli

REM --- Version = date du jour, format YYMM.dd (independant de la langue) ------
set "VER="
for /f %%D in ('powershell -NoProfile -Command "Get-Date -Format yyMM.dd"') do set "VER=%%D"
if not defined VER goto :err_date
set "FW_NAME=hid_firmware_v%VER%"
set "FW_OUT=%FW_DIR%\%FW_NAME%"

echo ============================================================
echo   S3-KBD - Compilation (SuperMini 4 Mo + PSRAM)
echo   arduino-cli : %ARDUINO_CLI%
echo   Version     : %FW_NAME%
echo ============================================================

echo [1/3] Generation de web_assets.h ...
call :regen
if errorlevel 1 goto :err_regen

echo [2/3] Compilation ...
"%ARDUINO_CLI%" compile --fqbn %FQBN% --output-dir "%BUILD_DIR%" "%SKETCH%"
if errorlevel 1 goto :err_build

echo [3/3] Copie dans firmware\%FW_NAME%\ ...
if exist "%FW_OUT%\" (echo     ^(remplace la version du jour^)) else mkdir "%FW_OUT%"
if errorlevel 1 goto :err_copy
call :publish ""            ""
if errorlevel 1 goto :err_copy
call :publish ".bootloader" ".bootloader"
if errorlevel 1 goto :err_copy
call :publish ".partitions" ".partitions"
if errorlevel 1 goto :err_copy

echo.
echo === Compilation OK ===
echo     Firmware : firmware\%FW_NAME%\  (.bin + .bootloader.bin + .partitions.bin)
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
REM Regenere l'app embarquee (WEB\Keyboard -> web_assets.h). BLOQUANT : sans
REM regeneration, le firmware embarquerait un site captif potentiellement perime.
REM Python est teste en l'executant : le python.exe "Microsoft Store" (WindowsApps)
REM passe "where" mais n'execute rien.
set "PY="
python -c "" >nul 2>&1 && set "PY=python"
if not defined PY (
  py -3 -c "" >nul 2>&1 && set "PY=py -3"
)
if not defined PY (
  echo     [X] Python 3 introuvable ^(ni "python" ni "py -3" ne s'execute^).
  exit /b 1
)
%PY% "%GEN%"
if errorlevel 1 exit /b 1
exit /b 0

:publish
REM %1 = suffixe de l'artefact arduino-cli, %2 = suffixe publie (avant .bin).
set "SRC=%BUILD_DIR%\hid_firmware.ino%~1.bin"
set "DST=%FW_OUT%\%FW_NAME%%~2.bin"
if not exist "%SRC%" (
  echo     [X] Artefact absent : %SRC%
  exit /b 1
)
copy /y "%SRC%" "%DST%" >nul || exit /b 1
echo     %FW_NAME%%~2.bin
exit /b 0

REM =============================== erreurs ====================================
:err_cli
echo [X] arduino-cli introuvable.
echo     Attendu : "%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
echo     Sinon : definir la variable ARDUINO_CLI ou l'ajouter au PATH.
set "RC=1"
goto :end

:err_date
echo [X] Impossible de lire la date (PowerShell indisponible ?).
set "RC=1"
goto :end

:err_regen
echo [X] Regeneration du site embarque impossible : compilation annulee.
echo     Sans elle, le firmware embarquerait un site captif perime.
echo     Python 3 requis dans le PATH (python.org, cocher "Add python.exe to PATH").
set "RC=1"
goto :end

:err_build
echo [X] Echec de la compilation (voir messages ci-dessus).
set "RC=1"
goto :end

:err_copy
echo [X] Echec de la copie vers "%FW_OUT%".
set "RC=1"
goto :end

:end
if defined DBLCLICK pause
endlocal & exit /b %RC%
