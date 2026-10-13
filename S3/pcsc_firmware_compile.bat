@echo off
REM ============================================================================
REM  pcsc_firmware_compile.bat
REM  Compile le firmware "lecteur de Carte Vitale" (sans televerser) et publie
REM  les binaires dans le sous-dossier firmware\pcsc_firmware_vYYMM.dd\ :
REM    pcsc_firmware_vYYMM.dd.bin              application          (0x10000)
REM    pcsc_firmware_vYYMM.dd.bootloader.bin   bootloader           (0x0)
REM    pcsc_firmware_vYYMM.dd.partitions.bin   table de partitions  (0x8000)
REM  Les trois fichiers vont ensemble : pcsc_firmware_flash.bat les ecrit aux
REM  bons offsets. Une 2e compilation le meme jour remplace la version du jour.
REM
REM  Carte : ESP32-S3-WROOM-1 N8R2 (DevKit 2 USB-C) = 8 Mo flash.
REM  -> FlashSize=8M + PartitionScheme=default_8MB : NE PAS flasher sur une
REM     SuperMini 4 Mo (elle bouclerait au demarrage).
REM  USBMode=default + CDCOnBoot=default : le sketch n'appelle jamais USB.begin(),
REM  le controleur USB-OTG reste libre pour l'USB HOST (lecteur de carte).
REM  CPUFreq=80 : sobriete (pas en dessous : PLL necessaire a l'USB).
REM  PSRAM=disabled (inutile), DebugLevel=none (UART0 = port protocole).
REM
REM  Usage :  pcsc_firmware_compile.bat
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
title Compil firmware PCSC (Carte Vitale)

REM --- Pause finale seulement si lance par double-clic (fenetre Explorer) ------
set "DBLCLICK="
echo %cmdcmdline% | find /i "%~0" >nul 2>&1 && set "DBLCLICK=1"

REM --- Dossiers (ce .bat est dans S3\) -----------------------------------------
set "S3_DIR=%~dp0"
set "SKETCH=%S3_DIR%pcsc_firmware"
set "BUILD_DIR=%S3_DIR%build\pcsc_firmware"
set "FW_DIR=%S3_DIR%firmware"

REM --- FQBN : N8R2 8 Mo, USB Host, 80 MHz --------------------------------------
set "FQBN=esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,CPUFreq=80,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled,DebugLevel=none"

call :find_cli
if errorlevel 1 goto :err_cli

REM --- Version = date du jour, format YYMM.dd (independant de la langue) ------
set "VER="
for /f %%D in ('powershell -NoProfile -Command "Get-Date -Format yyMM.dd"') do set "VER=%%D"
if not defined VER goto :err_date
set "FW_NAME=pcsc_firmware_v%VER%"
set "FW_OUT=%FW_DIR%\%FW_NAME%"

echo ============================================================
echo   PCSC (Carte Vitale) - Compilation (ESP32-S3 N8R2 8 Mo)
echo   arduino-cli : %ARDUINO_CLI%
echo   Version     : %FW_NAME%
echo ============================================================

echo [1/2] Compilation ...
"%ARDUINO_CLI%" compile --fqbn %FQBN% --output-dir "%BUILD_DIR%" "%SKETCH%"
if errorlevel 1 goto :err_build

echo [2/2] Copie dans firmware\%FW_NAME%\ ...
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
echo     Pour televerser : pcsc_firmware_flash.bat [COMx]
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

:publish
REM %1 = suffixe de l'artefact arduino-cli, %2 = suffixe publie (avant .bin).
set "SRC=%BUILD_DIR%\pcsc_firmware.ino%~1.bin"
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
