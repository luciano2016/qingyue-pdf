@echo off
setlocal

set ROOT=%~dp0..\..
set MUPDF=%ROOT%\third_party\mupdf-1.28.0-source
set OUT=%ROOT%\native\quietmupdf\out
set LIBDIR=%MUPDF%\platform\win32\x64\Release

if not exist "%OUT%" mkdir "%OUT%"

set VCVARS=E:\dev\Microsoft Visual Studio18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo Visual Studio vcvars64.bat not found.
  exit /b 1
)

call "%VCVARS%"
if errorlevel 1 exit /b %errorlevel%

cl /nologo /LD /O2 /MD /I"%MUPDF%\include" "%ROOT%\native\quietmupdf\quietmupdf.cpp" /Fe:"%OUT%\quietmupdf.dll" /Fo:"%OUT%\quietmupdf.obj" ^
  /link /NOLOGO /OPT:REF /OPT:ICF /DEF:"%ROOT%\native\quietmupdf\quietmupdf.def" ^
  /LIBPATH:"%LIBDIR%" ^
  libmupdf.lib libresources.lib libthirdparty.lib libharfbuzz.lib libpkcs7.lib libextract.lib libmubarcode.lib libzxing.lib libleptonica.lib libtesseract.lib ^
  user32.lib gdi32.lib shell32.lib advapi32.lib crypt32.lib ws2_32.lib






