@echo off
setlocal

set ROOT=%~dp0..\..

set VCVARS=E:\dev\Microsoft Visual Studio18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo Visual Studio vcvars64.bat not found.
  exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%

dumpbin /exports "%ROOT%\build\x64\Release\quietmupdf.dll" | findstr "quiet_new_shared_context quiet_clone_context fz_new_context_imp fz_open_document fz_register_document_handlers fz_new_pixmap_from_page_number fz_drop_context quiet_pdf_get_text_char_boxes"
