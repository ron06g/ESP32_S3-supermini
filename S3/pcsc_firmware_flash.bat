@echo off
REM ============================================================================
REM  pcsc_firmware_flash.bat
REM  Televerse le DERNIER firmware PCSC de firmware\ (sans recompiler) sur la
REM  carte ESP32-S3 N8R2, par sa prise "COM" (pont USB-serie CH343 sur UART0).
REM
REM  Firmware : le plus recent sous-dossier firmware\pcsc_firmware_vYYMM.dd\
REM  (produit par pcsc_firmware_compile.bat) : pcsc_firmware_vYYMM.dd.bin avec
REM  ses .bootloader.bin et .partitions.bin.
REM
REM  Port : auto-detection des PONTS USB-SERIE (VID 1A86 WCH/CH34x, 10C4 CP210x,
REM  0403 FTDI). Les ports Espressif (VID 303A) sont volontairement ECARTES :
REM  ce sont les modules S3-KBD (firmware HID) qu'il ne faut pas ecraser. La prise
REM  "USB" de la N8R2 sert d'USB HOST (lecteur de carte) : on ne flashe pas par
REM  elle. Plusieurs ponts branches -> choix du port, ou T = tous.
REM
REM  Usage :  pcsc_firmware_flash.bat [COMx ...] [dossier ou fichier.bin]
REM    - sans argument : port auto-detecte + dernier firmware.
REM    - COMx          : force le(s) port(s) (ex. pcsc_firmware_flash.bat COM12).
REM    - dossier       : force la version (ex. firmware\pcsc_firmware_v2610.14).
REM    - fichier.bin   : force l'application (compagnons dans le meme dossier).
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
title Flash firmware PCSC (Carte Vitale)

REM --- Pause finale seulement si lance par double-clic (fenetre Explorer) ------
set "DBLCLICK="
echo %cmdcmdline% | find /i "%~0" >nul 2>&1 && set "DBLCLICK=1"

REM --- Dossiers (ce .bat est dans S3\) -----------------------------------------
set "S3_DIR=%~dp0"
set "FW_DIR=%S3_DIR%firmware"

REM --- FQBN : doit rester IDENTIQUE a celui de pcsc_firmware_compile.bat -------
set "FQBN=esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,CPUFreq=80,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled,DebugLevel=none"

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
set "STAGE=%TEMP%\pcsc_flash"
if exist "%STAGE%\" rd /s /q "%STAGE%"
mkdir "%STAGE%" || goto :err_stage
for %%S in ("" ".bootloader" ".partitions") do (
  copy /y "%FW_BASE%%%~S.bin" "%STAGE%\" >nul || goto :err_stage
)
set "FW_UP=%STAGE%\%FW_SHOW%"

REM --- Port : argument, sinon auto-detection (ponts USB-serie) -----------------
if not defined PORTS call :detect_ports
if not defined PORTS goto :err_noport

echo ============================================================
echo   PCSC (Carte Vitale) - Flash firmware (ESP32-S3 N8R2 8 Mo)
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
echo === Upload OK ===
echo     La carte redemarre seule (auto-reset du pont USB-serie). Sinon : RESET.
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
REM Sous-dossier pcsc_firmware_vYYMM.dd : l'ordre alphabetique = l'ordre
REM chronologique. Le filtre exact ecarte tout autre dossier.
for /f "delims=" %%D in ('dir /b /ad /o:n "%FW_DIR%\pcsc_firmware_v*" 2^>nul ^| findstr /r /i /x "pcsc_firmware_v[0-9][0-9][0-9][0-9]\.[0-9][0-9]"') do set "FW=%FW_DIR%\%%D\%%D.bin"
exit /b 0

:detect_ports
REM Ports COM dont l'identifiant USB porte le VID d'un pont USB-serie (WCH 1A86,
REM Silicon Labs 10C4, FTDI 0403). Ecarte les modules Espressif (303A) et les
REM autres peripheriques serie. Resultat trie par numero de port.
set "NPORT=0"
for /f "tokens=1,* delims=|" %%P in ('powershell -NoProfile -Command "Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_1A86&PID_(7522|7523|5523|55D[2-9])|VID_10C4&PID_EA[67]0|VID_0403&PID_60(01|10|11|14|15)' -and $_.FriendlyName -match '\(COM\d+\)' } | ForEach-Object { [void]($_.FriendlyName -match '\((COM(\d+))\)'); [pscustomobject]@{ N = [int]$Matches[2]; L = $Matches[1] + '|' + $_.FriendlyName } } | Sort-Object N | ForEach-Object { $_.L }"') do (
  set /a NPORT+=1
  set "PORT_!NPORT!=%%P"
  set "DESC_!NPORT!=%%Q"
)
if %NPORT%==0 exit /b 0
if %NPORT%==1 (
  set "PORTS= %PORT_1%"
  exit /b 0
)
echo Plusieurs ponts USB-serie detectes :
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
echo     Lancer d'abord pcsc_firmware_compile.bat.
set "RC=1"
goto :end

:err_parts
echo [X] Fichiers compagnons manquants pour %FW% :
echo     %FW_BASE%.bootloader.bin
echo     %FW_BASE%.partitions.bin
echo     Recompiler avec pcsc_firmware_compile.bat.
set "RC=1"
goto :end

:err_stage
echo [X] Impossible de preparer la copie de travail "%STAGE%".
set "RC=1"
goto :end

:err_noport
echo [X] Aucun pont USB-serie detecte (ou choix de port invalide).
echo     Brancher la prise "COM" de la carte N8R2 (pas la prise "USB").
echo     Ou forcer le port : "%~nx0" COMx
set "RC=1"
goto :end

:err_upload
echo.
echo [X] Echec du televersement (%FAILS% port(s)).
echo     Mettre la carte en mode BOOT (maintenir BOOT, appuyer/relacher RESET,
echo     relacher BOOT) puis relancer ce script.
set "RC=1"
goto :end

:end
if defined STAGE if exist "%STAGE%\" rd /s /q "%STAGE%"
if defined DBLCLICK pause
endlocal & exit /b %RC%
