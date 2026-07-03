@echo off
setlocal

set ROOT=%~dp0..\..

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%

dumpbin /exports "%ROOT%\build\x64\Release\quietmupdf.dll" | findstr "fz_new_context_imp fz_open_document fz_register_document_handlers fz_new_pixmap_from_page_number fz_drop_context"
