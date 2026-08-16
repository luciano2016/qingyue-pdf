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

dumpbin /exports "%ROOT%\build\x64\Release\quietmupdf.dll" | findstr "quiet_new_shared_context quiet_clone_context quiet_shrink_shared_store quiet_open_document_shared_w quiet_pdf_get_page_bounds quiet_new_render_cookie quiet_abort_render_cookie quiet_drop_render_cookie quiet_new_pixmap_from_page_number_clip_cookie quiet_new_page_contents_display_list_cookie quiet_drop_display_list quiet_new_pixmap_from_display_list_clip_cookie fz_new_context_imp fz_open_document fz_register_document_handlers fz_device_bgr fz_new_pixmap_from_page_number fz_drop_context quiet_pdf_get_text_char_boxes"
