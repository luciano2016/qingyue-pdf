; Legacy NSIS installer retained for reference. The supported release path is build_setup.cmd.
Unicode true
ManifestDPIAware true
ManifestDPIAwareness PerMonitorV2
SetCompressor /SOLID lzma
RequestExecutionLevel user

!include MUI2.nsh
!include x64.nsh
!include WinMessages.nsh

!define APP_NAME "轻阅PDF"
!define APP_VERSION "2.0.9"
!define APP_VERSION_FULL "2.0.9.0"
!define APP_PUBLISHER "轻阅"
!define APP_EXE_NAME "轻阅PDF.exe"
!define APP_UNINSTALL_NAME "卸载轻阅PDF.exe"
!define APP_REG_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\QingYuePDF"
!define APP_INSTALL_REG_KEY "Software\QingYuePDF"
!define APP_PROG_ID "QingYuePDF.Document"
!define APP_OPENWITH_KEY "Software\Classes\Applications\${APP_EXE_NAME}"
!define APP_CAPABILITIES_KEY "Software\QingYuePDF\Capabilities"
!define APP_REGISTERED_NAME "QingYuePDF"
!define ROOT_DIR "${__FILEDIR__}\.."
!define SOURCE_EXE "${ROOT_DIR}\build\x64\Release\QuietPDF.Win32.exe"
!define SOURCE_DLL "${ROOT_DIR}\build\x64\Release\quietmupdf.dll"
!define SOURCE_VCRUNTIME140 "${ROOT_DIR}\build\setup-input\vcruntime140.dll"
!define SOURCE_LICENSE "${ROOT_DIR}\LICENSE"
!define SOURCE_NOTICES "${ROOT_DIR}\THIRD_PARTY_NOTICES.md"
!define SOURCE_MUPDF_LICENSES "${ROOT_DIR}\LICENSES\MUPDF_THIRD_PARTY_LICENSES.txt"
!define SOURCE_GPL "${ROOT_DIR}\LICENSES\GPL-3.0.txt"
!define APP_ICON "${ROOT_DIR}\assets\xiaoyue.ico"
!define SETUP_OUT "${ROOT_DIR}\dist\轻阅PDF-Setup-x64.exe"

Name "${APP_NAME}"
OutFile "${SETUP_OUT}"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "${APP_INSTALL_REG_KEY}" "InstallLocation"
BrandingText "${APP_NAME}"

Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "欢迎安装 ${APP_NAME}"
!define MUI_WELCOMEPAGE_TEXT "安装向导将引导你完成 ${APP_NAME} 的安装。$\r$\n$\r$\n建议在继续安装前关闭其它应用程序。"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE_NAME}"
!define MUI_FINISHPAGE_RUN_TEXT "运行 ${APP_NAME}"
!define MUI_FINISHPAGE_LINK "安装完成后可在开始菜单或桌面快捷方式打开 ${APP_NAME}。"
!define MUI_FINISHPAGE_LINK_LOCATION "$INSTDIR"

VIProductVersion "${APP_VERSION_FULL}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} 安装程序"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2026 ${APP_PUBLISHER}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

Function CloseRunningApp
  FindWindow $0 "XiaoYuePDFWindow"
  StrCmp $0 0 done

  MessageBox MB_ICONINFORMATION|MB_OKCANCEL "检测到 ${APP_NAME} 正在运行。$\r$\n$\r$\n安装前需要关闭旧程序。点击“确定”自动关闭后继续安装，或点击“取消”退出安装。" IDOK close IDCANCEL cancel

  close:
    StrCpy $1 0
    close_loop:
      FindWindow $0 "XiaoYuePDFWindow"
      StrCmp $0 0 done
      SendMessage $0 ${WM_CLOSE} 0 0
      Sleep 300
      IntOp $1 $1 + 1
      IntCmp $1 20 still_running close_loop close_loop

  still_running:
    MessageBox MB_ICONEXCLAMATION|MB_RETRYCANCEL "${APP_NAME} 仍在运行。$\r$\n$\r$\n请手动关闭程序后点击“重试”，或点击“取消”退出安装。" IDRETRY close IDCANCEL cancel

  cancel:
    Abort

  done:
FunctionEnd

Function EnsureInstallFilesWritable
  retry:
    ClearErrors
    Delete "$INSTDIR\${APP_EXE_NAME}"
    Delete "$INSTDIR\quietmupdf.dll"
    Delete "$INSTDIR\vcruntime140.dll"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.md"
  Delete "$INSTDIR\MUPDF_THIRD_PARTY_LICENSES.txt"
  Delete "$INSTDIR\GPL-3.0.txt"
    IfErrors locked done

  locked:
    Call CloseRunningApp
    nsExec::ExecToStack '"$SYSDIR\taskkill.exe" /IM "${APP_EXE_NAME}" /T /F'
    Pop $0
    Pop $1
    Sleep 600
    ClearErrors
    Delete "$INSTDIR\${APP_EXE_NAME}"
    Delete "$INSTDIR\quietmupdf.dll"
    Delete "$INSTDIR\vcruntime140.dll"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.md"
  Delete "$INSTDIR\MUPDF_THIRD_PARTY_LICENSES.txt"
  Delete "$INSTDIR\GPL-3.0.txt"
    IfErrors ask done

  ask:
    MessageBox MB_ICONEXCLAMATION|MB_RETRYCANCEL "无法更新 ${APP_NAME}，旧程序可能仍在运行。$\r$\n$\r$\n请关闭 ${APP_NAME} 后点击“重试”，或点击“取消”退出安装。" IDRETRY retry IDCANCEL cancel

  cancel:
    Abort

  done:
FunctionEnd

Section "${APP_NAME}" SecMain
  SectionIn RO
  Call CloseRunningApp
  SetOutPath "$INSTDIR"
  Call EnsureInstallFilesWritable
  File "/oname=${APP_EXE_NAME}" "${SOURCE_EXE}"
  File "${SOURCE_DLL}"
  File "${SOURCE_VCRUNTIME140}"
  File "${SOURCE_LICENSE}"
  File "${SOURCE_NOTICES}"
  File "${SOURCE_MUPDF_LICENSES}"
  File "${SOURCE_GPL}"

  WriteUninstaller "$INSTDIR\${APP_UNINSTALL_NAME}"
  WriteRegStr HKCU "${APP_INSTALL_REG_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${APP_REG_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${APP_REG_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${APP_REG_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${APP_REG_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE_NAME}"
  WriteRegStr HKCU "${APP_REG_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${APP_REG_KEY}" "UninstallString" '"$INSTDIR\${APP_UNINSTALL_NAME}"'
  WriteRegDWORD HKCU "${APP_REG_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${APP_REG_KEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${APP_REG_KEY}" "EstimatedSize" 18000
  WriteRegStr HKCU "${APP_OPENWITH_KEY}" "FriendlyAppName" "${APP_NAME}"
  WriteRegStr HKCU "${APP_OPENWITH_KEY}\DefaultIcon" "" "$INSTDIR\${APP_EXE_NAME},0"
  WriteRegStr HKCU "${APP_OPENWITH_KEY}\shell\open\command" "" '"$INSTDIR\${APP_EXE_NAME}" "%1"'
  WriteRegStr HKCU "${APP_OPENWITH_KEY}\SupportedTypes" ".pdf" ""
  WriteRegStr HKCU "Software\Classes\.pdf\OpenWithProgids" "${APP_PROG_ID}" ""
  WriteRegStr HKCU "Software\Classes\${APP_PROG_ID}" "" "${APP_NAME}"
  WriteRegStr HKCU "Software\Classes\${APP_PROG_ID}\DefaultIcon" "" "$INSTDIR\${APP_EXE_NAME},0"
  WriteRegStr HKCU "Software\Classes\${APP_PROG_ID}\shell\open\command" "" '"$INSTDIR\${APP_EXE_NAME}" "%1"'
  WriteRegStr HKCU "${APP_CAPABILITIES_KEY}" "ApplicationName" "${APP_NAME}"
  WriteRegStr HKCU "${APP_CAPABILITIES_KEY}" "ApplicationDescription" "${APP_NAME}"
  WriteRegStr HKCU "${APP_CAPABILITIES_KEY}\FileAssociations" ".pdf" "${APP_PROG_ID}"
  WriteRegStr HKCU "Software\RegisteredApplications" "${APP_REGISTERED_NAME}" "${APP_CAPABILITIES_KEY}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE_NAME}" "" "$INSTDIR\${APP_EXE_NAME}" 0
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\卸载 ${APP_NAME}.lnk" "$INSTDIR\${APP_UNINSTALL_NAME}" "" "$INSTDIR\${APP_UNINSTALL_NAME}" 0
SectionEnd

Section "创建桌面快捷方式" SecDesktop
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE_NAME}" "" "$INSTDIR\${APP_EXE_NAME}" 0
SectionEnd

LangString DESC_SecMain ${LANG_SIMPCHINESE} "安装 ${APP_NAME} 主程序。"
LangString DESC_SecDesktop ${LANG_SIMPCHINESE} "在桌面创建 ${APP_NAME} 快捷方式。"
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_SecMain)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_SecDesktop)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "un.卸载 ${APP_NAME} 程序文件" SecProgramFiles
  SectionIn RO
  Call un.CloseRunningApp
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\卸载 ${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  Delete "$INSTDIR\quietmupdf.dll"
  Delete "$INSTDIR\vcruntime140.dll"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.md"
  Delete "$INSTDIR\MUPDF_THIRD_PARTY_LICENSES.txt"
  Delete "$INSTDIR\GPL-3.0.txt"
  Delete "$INSTDIR\${APP_EXE_NAME}"
  Delete "$INSTDIR\${APP_UNINSTALL_NAME}"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${APP_REG_KEY}"
  DeleteRegKey HKCU "${APP_INSTALL_REG_KEY}"
  DeleteRegKey HKCU "${APP_OPENWITH_KEY}"
  DeleteRegKey HKCU "${APP_CAPABILITIES_KEY}"
  DeleteRegKey HKCU "Software\Classes\${APP_PROG_ID}"
  DeleteRegValue HKCU "Software\Classes\.pdf\OpenWithProgids" "${APP_PROG_ID}"
  DeleteRegValue HKCU "Software\RegisteredApplications" "${APP_REGISTERED_NAME}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section /o "un.删除应用配置数据（阅读进度、最近文档、主题设置）" un.SecUserData
  RMDir /r "$APPDATA\QingYuePDF"
SectionEnd

Function un.CloseRunningApp
  FindWindow $0 "XiaoYuePDFWindow"
  StrCmp $0 0 done

  MessageBox MB_ICONINFORMATION|MB_OKCANCEL "检测到 ${APP_NAME} 正在运行。$\r$\n$\r$\n卸载前需要关闭程序。点击“确定”自动关闭后继续卸载，或点击“取消”退出卸载。" IDOK close IDCANCEL cancel

  close:
    StrCpy $1 0
    close_loop:
      FindWindow $0 "XiaoYuePDFWindow"
      StrCmp $0 0 done
      SendMessage $0 ${WM_CLOSE} 0 0
      Sleep 300
      IntOp $1 $1 + 1
      IntCmp $1 20 still_running close_loop close_loop

  still_running:
    MessageBox MB_ICONEXCLAMATION|MB_RETRYCANCEL "${APP_NAME} 仍在运行。$\r$\n$\r$\n请手动关闭程序后点击“重试”，或点击“取消”退出卸载。" IDRETRY close IDCANCEL cancel

  cancel:
    Abort

  done:
FunctionEnd
