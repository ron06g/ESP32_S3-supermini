@echo off
REM ==========================================================================
REM  serve.bat  —  Lance le serveur HTTPS local du dossier WEB/ (mock).
REM  Genere le certificat auto-signe (WEB\ssl\) au premier lancement.
REM  Web Bluetooth exige HTTPS ou localhost : ce serveur fournit le HTTPS.
REM
REM  Usage :  serve.bat            (port 8443 par defaut)
REM           serve.bat 9443       (port au choix)
REM ==========================================================================
setlocal
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo [serve] ERREUR : Python introuvable dans le PATH.
  echo         Installez Python 3 ^(python.org^) ou ajoutez-le au PATH.
  pause
  exit /b 1
)

set "PORT=%~1"
if "%PORT%"=="" set "PORT=8443"

echo [serve] Demarrage du serveur HTTPS sur le port %PORT% ...
python "%~dp0serve.py" %PORT%

endlocal
