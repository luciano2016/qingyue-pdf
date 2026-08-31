@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
if not defined DLL_PATH set "DLL_PATH=%ROOT%\build\x64\Release\quietmupdf.dll"

where dumpbin.exe >nul 2>&1
if errorlevel 1 (
  if not defined VSWHERE set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not defined VCVARS64 if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do if not defined VCVARS64 set "VCVARS64=%%I"
  if not defined VCVARS64 (
    echo Visual Studio C++ build tools were not found.
    echo Set VCVARS64 to the full path of vcvars64.bat.
    exit /b 1
  )
  call "%VCVARS64%" >nul
  if errorlevel 1 exit /b %errorlevel%
)

if not exist "%DLL_PATH%" (
  echo DLL was not found at "%DLL_PATH%".
  exit /b 1
)

dumpbin /exports "%DLL_PATH%" | findstr "quiet_new_shared_context quiet_clone_context quiet_shrink_shared_store quiet_open_document_shared_w quiet_pdf_get_page_bounds quiet_new_render_cookie quiet_abort_render_cookie quiet_drop_render_cookie quiet_new_pixmap_from_page_number_clip_cookie quiet_new_page_contents_display_list_cookie quiet_drop_display_list quiet_new_pixmap_from_display_list_clip_cookie fz_new_context_imp fz_open_document fz_register_document_handlers fz_device_bgr fz_new_pixmap_from_page_number fz_drop_context quiet_pdf_get_text_char_boxes"
