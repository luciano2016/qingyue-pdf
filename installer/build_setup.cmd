@echo off
setlocal EnableExtensions
cd /d "%~dp0.."

set "ROOT=%CD%"
set "MSBUILD=E:\dev\Microsoft Visual Studio18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" set "MSBUILD=msbuild"
set "MAKECAB=%SystemRoot%\System32\makecab.exe"
if not exist "%MAKECAB%" (
  echo makecab.exe not found.
  exit /b 1
)

if not exist "%ROOT%\dist" mkdir "%ROOT%\dist"
if not exist "%ROOT%\build\setup-input" mkdir "%ROOT%\build\setup-input"
if not exist "%ROOT%\build\setup-payload" mkdir "%ROOT%\build\setup-payload"

echo Building QuietPDF Release x64...
"%MSBUILD%" "%ROOT%\QuietPDF.Win32.sln" /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%

copy /y "%ROOT%\build\x64\Release\QuietPDF.Win32.exe" "%ROOT%\build\setup-input\QuietPDF.Win32.exe" >nul
copy /y "%ROOT%\build\x64\Release\quietmupdf.dll" "%ROOT%\build\setup-input\quietmupdf.dll" >nul
copy /y "E:\dev\Microsoft Visual Studio18\BuildTools\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT\vcruntime140.dll" "%ROOT%\build\setup-input\vcruntime140.dll" >nul
if errorlevel 1 (
  echo Required runtime file was not found.
  exit /b 1
)

del /q "%ROOT%\build\setup-payload\quietpdf-payload.cab" 2>nul
"%MAKECAB%" /f "%ROOT%\installer\payload.ddf"
del /q "%ROOT%\setup.inf" "%ROOT%\setup.rpt" 2>nul
if errorlevel 1 exit /b %errorlevel%
del /q "%ROOT%\setup.inf" "%ROOT%\setup.rpt" 2>nul

"%MSBUILD%" "%ROOT%\installer\XiaoYuePDF.Setup.vcxproj" /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\installer\post_build_setup.ps1"
if errorlevel 1 exit /b %errorlevel%

echo Built custom native installer.
