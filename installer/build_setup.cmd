@echo off
setlocal EnableExtensions
cd /d "%~dp0.."

set "ROOT=%CD%"
if not defined VSWHERE set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not defined VCVARS64 if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do if not defined VCVARS64 set "VCVARS64=%%I"
if defined VCVARS64 (
  call "%VCVARS64%"
  if errorlevel 1 exit /b %errorlevel%
)

if not defined MSBUILD if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do if not defined MSBUILD set "MSBUILD=%%I"
if not defined MSBUILD for /f "delims=" %%I in ('where msbuild.exe 2^>nul') do if not defined MSBUILD set "MSBUILD=%%I"
if not defined MSBUILD (
  echo MSBuild was not found.
  echo Install Visual Studio Build Tools or set MSBUILD to MSBuild.exe.
  exit /b 1
)

if not defined SKIP_NATIVE_BUILD (
  echo Building native MuPDF adapter...
  call "%ROOT%\native\quietmupdf\build.cmd"
  if errorlevel 1 exit /b %errorlevel%
)

set "MAKECAB=%SystemRoot%\System32\makecab.exe"
if not exist "%MAKECAB%" (
  echo makecab.exe was not found.
  exit /b 1
)

if not exist "%ROOT%\dist" mkdir "%ROOT%\dist"
if not exist "%ROOT%\build\setup-input" mkdir "%ROOT%\build\setup-input"
if not exist "%ROOT%\build\setup-payload" mkdir "%ROOT%\build\setup-payload"

echo Building QuietPDF Release x64...
"%MSBUILD%" "%ROOT%\QuietPDF.Win32.sln" /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%

if not exist "%ROOT%\build\x64\Release\QuietPDF.Win32.exe" (
  echo Main executable was not produced.
  exit /b 1
)
if not exist "%ROOT%\native\quietmupdf\out\quietmupdf.dll" (
  echo quietmupdf.dll was not produced.
  exit /b 1
)

copy /y "%ROOT%\build\x64\Release\QuietPDF.Win32.exe" "%ROOT%\build\setup-input\QuietPDF.Win32.exe" >nul
copy /y "%ROOT%\native\quietmupdf\out\quietmupdf.dll" "%ROOT%\build\setup-input\quietmupdf.dll" >nul

if not defined VCRUNTIME140 if defined VCToolsRedistDir for /d %%D in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do if not defined VCRUNTIME140 if exist "%%~fD\vcruntime140.dll" set "VCRUNTIME140=%%~fD\vcruntime140.dll"
if not defined VCRUNTIME140 (
  echo vcruntime140.dll was not found in the Visual C++ redistributable directory.
  echo Set VCRUNTIME140 to the full path of the x64 runtime DLL.
  exit /b 1
)
copy /y "%VCRUNTIME140%" "%ROOT%\build\setup-input\vcruntime140.dll" >nul
copy /y "%ROOT%\LICENSE" "%ROOT%\build\setup-input\LICENSE" >nul
copy /y "%ROOT%\THIRD_PARTY_NOTICES.md" "%ROOT%\build\setup-input\THIRD_PARTY_NOTICES.md" >nul
copy /y "%ROOT%\LICENSES\MUPDF_THIRD_PARTY_LICENSES.txt" "%ROOT%\build\setup-input\MUPDF_THIRD_PARTY_LICENSES.txt" >nul
copy /y "%ROOT%\LICENSES\GPL-3.0.txt" "%ROOT%\build\setup-input\GPL-3.0.txt" >nul
if errorlevel 1 exit /b %errorlevel%

echo Building lightweight uninstaller...
"%MSBUILD%" "%ROOT%\installer\QingYuePDF.Uninstaller.vcxproj" /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%

del /q "%ROOT%\build\setup-payload\quietpdf-payload.cab" 2>nul
"%MAKECAB%" /f "%ROOT%\installer\payload.ddf"
if errorlevel 1 exit /b %errorlevel%
del /q "%ROOT%\setup.inf" "%ROOT%\setup.rpt" 2>nul

"%MSBUILD%" "%ROOT%\installer\XiaoYuePDF.Setup.vcxproj" /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\installer\post_build_setup.ps1"
if errorlevel 1 exit /b %errorlevel%

echo Built custom native installer.
