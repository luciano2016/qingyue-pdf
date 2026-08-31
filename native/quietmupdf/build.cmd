@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
if not defined MUPDF_ROOT set "MUPDF_ROOT=%ROOT%\third_party\mupdf-1.28.0-source"
if not defined MUPDF_LIBDIR set "MUPDF_LIBDIR=%MUPDF_ROOT%\platform\win32\x64\Release"
set "OUT=%ROOT%\native\quietmupdf\out"

if not exist "%MUPDF_ROOT%\include\mupdf\fitz.h" (
  echo MuPDF source was not found at "%MUPDF_ROOT%".
  echo Run scripts\fetch-mupdf.ps1 and build MuPDF Release x64 first.
  exit /b 1
)
if not exist "%MUPDF_LIBDIR%\libmupdf.lib" (
  echo MuPDF Release x64 libraries were not found at "%MUPDF_LIBDIR%".
  echo See docs\BUILDING.md for the required MuPDF build step.
  exit /b 1
)

where cl.exe >nul 2>&1
if errorlevel 1 (
  if not defined VSWHERE set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not defined VCVARS64 if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do if not defined VCVARS64 set "VCVARS64=%%I"
  if not defined VCVARS64 if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  if not defined VCVARS64 if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  if not defined VCVARS64 (
    echo Visual Studio C++ build tools were not found.
    echo Set VCVARS64 to the full path of vcvars64.bat.
    exit /b 1
  )
  call "%VCVARS64%"
  if errorlevel 1 exit /b %errorlevel%
)

if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /c /O2 /MD /I"%MUPDF_ROOT%\include" "%ROOT%\native\quietmupdf\quietmupdf.cpp" /Fo:"%OUT%\quietmupdf.obj"
if errorlevel 1 exit /b %errorlevel%

cl /nologo /c /O2 /MD /I"%MUPDF_ROOT%\include" /I"%MUPDF_ROOT%\thirdparty\freetype\include" "%ROOT%\native\quietmupdf\mupdf_load_system_font.c" /Fo:"%OUT%\mupdf_load_system_font.obj"
if errorlevel 1 exit /b %errorlevel%

link /DLL /NOLOGO /OPT:REF /OPT:ICF /DEF:"%ROOT%\native\quietmupdf\quietmupdf.def" /OUT:"%OUT%\quietmupdf.dll" ^
  "%OUT%\quietmupdf.obj" "%OUT%\mupdf_load_system_font.obj" ^
  /LIBPATH:"%MUPDF_LIBDIR%" ^
  libmupdf.lib libresources.lib libthirdparty.lib libharfbuzz.lib libpkcs7.lib libextract.lib libmubarcode.lib libzxing.lib libleptonica.lib libtesseract.lib ^
  user32.lib gdi32.lib shell32.lib advapi32.lib crypt32.lib ws2_32.lib
if errorlevel 1 exit /b %errorlevel%

echo Built "%OUT%\quietmupdf.dll".
