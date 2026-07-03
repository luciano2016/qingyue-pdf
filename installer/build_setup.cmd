@echo off
setlocal
cd /d "%~dp0.."

if not exist dist mkdir dist

"C:\Users\10726\AppData\Local\tauri\NSIS\Bin\makensis.exe" /INPUTCHARSET UTF8 /V2 "%CD%\installer\xiaoyue-modern.nsi"
if errorlevel 1 exit /b %errorlevel%

echo Built NSIS installer.
