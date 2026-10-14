@echo off
REM ============================================================================
REM  build_exe.bat
REM  Construit l'executable autonome du terminal graphique :
REM      S3\pcsc_firmware\tools\dist\pcsc_term.exe
REM  (un seul fichier, sans console, Python inutile sur le poste cible).
REM
REM  Prerequis (poste de construction uniquement) :
REM      python -m pip install pyserial pyinstaller
REM  Intermediaires dans tools\build\ (ignore par git), exe dans tools\dist\
REM  (ignore par git : a distribuer a part).
REM
REM  Usage :  build_exe.bat
REM ============================================================================
setlocal EnableExtensions
chcp 65001 >nul
title Construction de pcsc_term.exe

set "DBLCLICK="
echo %cmdcmdline% | find /i "%~0" >nul 2>&1 && set "DBLCLICK=1"

set "TOOLS=%~dp0"
set "SRC=%TOOLS%pcsc_term.py"
set "WORK=%TOOLS%build"
set "DIST=%TOOLS%dist"
set "ICO=%WORK%\pcsc_term.ico"

REM --- Python : "python" s'il s'execute, sinon "py -3" ---------------------------
set "PY="
python -c "" >nul 2>&1 && set "PY=python"
if not defined PY (
  py -3 -c "" >nul 2>&1 && set "PY=py -3"
)
if not defined PY goto :err_py

%PY% -c "import serial" >nul 2>&1 || goto :err_serial
%PY% -m PyInstaller --version >nul 2>&1 || goto :err_pyi

echo ============================================================
echo   pcsc_term.exe - terminal graphique du lecteur Carte Vitale
echo ============================================================
if not exist "%WORK%\" mkdir "%WORK%"

echo [1/2] Icone ...
%PY% "%SRC%" --make-icon "%ICO%"
if errorlevel 1 goto :err_build

echo [2/2] PyInstaller ...
%PY% -m PyInstaller --noconfirm --clean --onefile --windowed --name pcsc_term ^
  --icon "%ICO%" --exclude-module PIL --exclude-module numpy ^
  --distpath "%DIST%" --workpath "%WORK%" --specpath "%WORK%" "%SRC%"
if errorlevel 1 goto :err_build

echo.
echo === OK : %DIST%\pcsc_term.exe ===
echo     Lancer :  pcsc_term.exe          (port COM reel)
echo               pcsc_term.exe --demo   (module simule, sans materiel)
set "RC=0"
goto :end

:err_py
echo [X] Python 3 introuvable (ni "python" ni "py -3").
set "RC=1"
goto :end

:err_serial
echo [X] pyserial absent :  %PY% -m pip install pyserial
set "RC=1"
goto :end

:err_pyi
echo [X] PyInstaller absent :  %PY% -m pip install pyinstaller
set "RC=1"
goto :end

:err_build
echo [X] Echec de la construction (voir messages ci-dessus).
set "RC=1"
goto :end

:end
if defined DBLCLICK pause
endlocal & exit /b %RC%
