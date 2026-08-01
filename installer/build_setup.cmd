@echo off
setlocal
cd /d "%~dp0.."

if not exist dist mkdir dist

set MAKENSIS=%LOCALAPPDATA%\tauri\NSIS\Bin\makensis.exe
if not exist "%MAKENSIS%" set MAKENSIS=C:\Users\10726\AppData\Local\tauri\NSIS\Bin\makensis.exe
if not exist "%MAKENSIS%" set MAKENSIS=C:\Program Files\NSIS\makensis.exe
if not exist "%MAKENSIS%" set MAKENSIS=C:\Program Files (x86)\NSIS\makensis.exe
if not exist "%MAKENSIS%" (
  echo makensis.exe not found. Install NSIS or Tauri NSIS bundle first.
  exit /b 1
)

"%MAKENSIS%" /INPUTCHARSET UTF8 /V2 "%CD%\installer\xiaoyue-modern.nsi"
if errorlevel 1 exit /b %errorlevel%

powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\installer\post_build_setup.ps1"
if errorlevel 1 exit /b %errorlevel%

echo Built NSIS installer.
