@echo off
REM ============================================================================
REM  hid_firmware_flash.bat
REM  Televerse le DERNIER firmware de firmware\ (sans recompiler) sur le module.
REM
REM  Firmware : le plus recent sous-dossier firmware\hid_firmware_vYYMM.dd\
REM  (produit par hid_firmware_compile.bat) : hid_firmware_vYYMM.dd.bin avec
REM  ses .bootloader.bin et .partitions.bin.
REM  Ecrit bootloader + partitions + application : la NVS n'est PAS effacee
REM  (reglages, passkey, appairages conserves).
REM
REM  Port : auto-detection des peripheriques Espressif (USB VID 303A), que le
REM  module soit en firmware (port COM actif) ou en ROM (mode BOOT). Plusieurs
REM  modules branches -> choix du port, ou T = tous.
REM
REM  Usage :  hid_firmware_flash.bat [COMx ...] [dossier ou fichier.bin]
REM    - sans argument : port auto-detecte + dernier firmware.
REM    - COMx          : force le(s) port(s) (ex. hid_firmware_flash.bat COM5 COM9).
REM    - dossier       : force la version (ex. firmware\hid_firmware_v2609.27).
REM    - fichier.bin   : force l'application (compagnons dans le meme dossier).
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
title Flash firmware S3-KBD

REM --- Pause finale seulement si lance par double-clic (fenetre Explorer) ------
set "DBLCLICK="
echo %cmdcmdline% | find /i "%~0" >nul 2>&1 && set "DBLCLICK=1"

REM --- Dossiers (ce .bat est dans S3\) -----------------------------------------
set "S3_DIR=%~dp0"
set "FW_DIR=%S3_DIR%firmware"

REM --- FQBN : doit rester IDENTIQUE a celui de hid_firmware_compile.bat --------
set "FQBN=esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled"

call :find_cli
if errorlevel 1 goto :err_cli

REM --- Arguments (ordre libre) : COMx = port, sinon = dossier/fichier firmware --
set "PORTS="
set "FW="
for %%A in (%*) do call :parse_arg "%%~A"

REM --- Firmware : argument, sinon le plus recent de firmware\ ------------------
if not defined FW call :find_latest
if not defined FW goto :err_nofw
if not exist "%FW%" goto :err_nofw
set "FW_BASE=%FW:~0,-4%"
if not exist "%FW_BASE%.bootloader.bin" goto :err_parts
if not exist "%FW_BASE%.partitions.bin" goto :err_parts
for %%F in ("%FW%") do (
  set "FW_SHOW=%%~nxF"
  set "FW_DATE=%%~tF"
  set "FW_SIZE=%%~zF"
)

REM --- Copie de travail : le wrapper flasher du coeur ecrit des *_flashed.bin --
REM     (references de reflash rapide) a cote des binaires -> firmware\ intact.
set "STAGE=%TEMP%\s3kbd_flash"
if exist "%STAGE%\" rd /s /q "%STAGE%"
mkdir "%STAGE%" || goto :err_stage
for %%S in ("" ".bootloader" ".partitions") do (
  copy /y "%FW_BASE%%%~S.bin" "%STAGE%\" >nul || goto :err_stage
)
set "FW_UP=%STAGE%\%FW_SHOW%"

REM --- Port : argument, sinon auto-detection (VID Espressif 303A) --------------
if not defined PORTS call :detect_ports
if not defined PORTS goto :err_noport

echo ============================================================
echo   S3-KBD - Flash firmware (SuperMini 4 Mo + PSRAM)
echo   arduino-cli : %ARDUINO_CLI%
echo   Firmware    : %FW_SHOW%  (%FW_SIZE% o, %FW_DATE%)
echo   Port(s)     :%PORTS%
echo ============================================================

set "FAILS=0"
for %%P in (%PORTS%) do (
  echo.
  echo Televersement sur %%P ...
  REM Pas de reference d'un autre module : flash complet a chaque port.
  del /q "%STAGE%\*_flashed.bin" >nul 2>&1
  "%ARDUINO_CLI%" upload --fqbn %FQBN% -p %%P --input-file "%FW_UP%"
  if errorlevel 1 (
    set /a FAILS+=1
    echo [X] Echec sur %%P
  ) else (
    echo [OK] %%P
  )
)
if not "%FAILS%"=="0" goto :err_upload

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

:parse_arg
REM COMx -> ajoute au(x) port(s) ; dossier de version -> son .bin ; sinon fichier.
set "ARG=%~1"
if "%ARG:~-1%"=="\" set "ARG=%ARG:~0,-1%"
if /i "%ARG:~0,3%"=="COM" (
  set "PORTS=%PORTS% %ARG%"
  exit /b 0
)
for %%F in ("%ARG%") do (
  if exist "%%~fF\" (set "FW=%%~fF\%%~nxF.bin") else set "FW=%%~fF"
)
exit /b 0

:find_latest
REM Sous-dossier hid_firmware_vYYMM.dd : l'ordre alphabetique = l'ordre
REM chronologique. Le filtre exact ecarte tout autre dossier.
for /f "delims=" %%D in ('dir /b /ad /o:n "%FW_DIR%\hid_firmware_v*" 2^>nul ^| findstr /r /i /x "hid_firmware_v[0-9][0-9][0-9][0-9]\.[0-9][0-9]"') do set "FW=%FW_DIR%\%%D\%%D.bin"
exit /b 0

:detect_ports
REM Ports COM dont l'identifiant USB porte VID_303A (Espressif) : module en
REM firmware (CDC TinyUSB) ou en ROM (USB JTAG/serial). Ecarte les autres
REM peripheriques serie (ex. COM3). Resultat trie par numero de port.
set "NPORT=0"
for /f "tokens=1,* delims=|" %%P in ('powershell -NoProfile -Command "Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_303A' -and $_.FriendlyName -match '\(COM\d+\)' } | ForEach-Object { [void]($_.FriendlyName -match '\((COM(\d+))\)'); [pscustomobject]@{ N = [int]$Matches[2]; L = $Matches[1] + '|' + $_.FriendlyName } } | Sort-Object N | ForEach-Object { $_.L }"') do (
  set /a NPORT+=1
  set "PORT_!NPORT!=%%P"
  set "DESC_!NPORT!=%%Q"
)
if %NPORT%==0 exit /b 0
if %NPORT%==1 (
  set "PORTS= %PORT_1%"
  exit /b 0
)
echo Plusieurs modules detectes :
for /l %%i in (1,1,%NPORT%) do echo   %%i^) !PORT_%%i!  - !DESC_%%i!
echo   T^) tous
set "SEL=1"
set /p "SEL=Port a flasher [1-%NPORT%, T=tous, defaut 1] : "
if /i "%SEL%"=="T" (
  for /l %%i in (1,1,%NPORT%) do set "PORTS=!PORTS! !PORT_%%i!"
  exit /b 0
)
set "PORTS= !PORT_%SEL%!"
if "%PORTS%"==" " set "PORTS="
exit /b 0

REM =============================== erreurs ====================================
:err_cli
echo [X] arduino-cli introuvable.
echo     Attendu : "%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
echo     Sinon : definir la variable ARDUINO_CLI ou l'ajouter au PATH.
set "RC=1"
goto :end

:err_nofw
echo [X] Aucun firmware a televerser.
if defined FW (echo     Fichier introuvable : %FW%) else echo     Aucune version dans : %FW_DIR%
echo     Lancer d'abord hid_firmware_compile.bat.
set "RC=1"
goto :end

:err_parts
echo [X] Fichiers compagnons manquants pour %FW% :
echo     %FW_BASE%.bootloader.bin
echo     %FW_BASE%.partitions.bin
echo     Recompiler avec hid_firmware_compile.bat.
set "RC=1"
goto :end

:err_stage
echo [X] Impossible de preparer la copie de travail "%STAGE%".
set "RC=1"
goto :end

:err_noport
echo [X] Aucun module ESP32-S3 detecte (ou choix de port invalide).
echo     Un module dont le port COM est desactive (flag serial off) n'expose
echo     que le HID : le passer en mode BOOT (maintenir BOOT, appuyer/relacher
echo     RESET, relacher BOOT) puis relancer ce script.
echo     Ou forcer le port : "%~nx0" COMx
set "RC=1"
goto :end

:err_upload
echo.
echo [X] Echec du televersement (%FAILS% port(s)).
echo     Mettre la carte en mode BOOT (maintenir BOOT, appuyer/relacher RESET,
echo     relacher BOOT) : elle reapparait souvent sur un AUTRE port COM,
echo     puis relancer ce script (nouvelle detection).
set "RC=1"
goto :end

:end
if defined STAGE if exist "%STAGE%\" rd /s /q "%STAGE%"
if defined DBLCLICK pause
endlocal & exit /b %RC%
