; SPDX-License-Identifier: Apache-2.0
; Copyright 2025 The Nesva Authors
;
; Per-user Windows installer for Nesva, built with NSIS.
;
; It installs into %LOCALAPPDATA%\Programs\Nesva and never asks for elevation:
; Nesva is a per-user application whose profile, keyring and schedule all belong
; to one account, so an all-users install would only put the binaries somewhere
; the user cannot write. The deployed Qt runtime, the three executables and the
; notes come from the staged directory passed as /DSTAGE.
;
; Build:
;   makensis /DVERSION=0.6.2 /DSTAGE=C:\stage /DOUTFILE=C:\out\nesva-setup.exe nesva.nsi

Unicode true

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef STAGE
  !error "STAGE must be defined: the directory holding nesva.exe and the Qt runtime"
!endif
!ifndef OUTFILE
  !define OUTFILE "nesva-setup.exe"
!endif

!define APP_NAME "Nesva"
!define APP_PUBLISHER "The Nesva Authors"
!define APP_URL "https://github.com/Linux-Alex/Nesva"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Nesva"

; The setup executable, the installer pages and the uninstaller use the Nesva
; icon. NSIS resolves a relative MUI_ICON against the script's own directory, so
; this default works from any working directory; the Release workflow can still
; override it with /DICON.
!ifndef ICON
  !define ICON "..\..\assets\icons\nesva.ico"
!endif
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\Nesva"
InstallDirRegKey HKCU "Software\Nesva" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Apache-2.0"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\nesva.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Nesva"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README-windows.md"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the Windows notes (background agent, autostart)"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Nesva (required)" SecMain
  SectionIn RO
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  ; Everything that was staged: the three executables, the Qt runtime and its
  ; plugin directories, the licence and the notes.
  File /r "${STAGE}\*"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\Nesva"
  CreateShortcut "$SMPROGRAMS\Nesva\Nesva.lnk" "$INSTDIR\nesva.exe"
  CreateShortcut "$SMPROGRAMS\Nesva\Uninstall Nesva.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "Software\Nesva" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "${APP_URL}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\nesva.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop shortcut" SecDesktop
  SetShellVarContext current
  CreateShortcut "$DESKTOP\Nesva.lnk" "$INSTDIR\nesva.exe"
SectionEnd

LangString DESC_SecMain ${LANG_ENGLISH} "Nesva, the command-line and background agent tools, and the Qt runtime they need. Installs for the current user only; no administrator rights."
LangString DESC_SecDesktop ${LANG_ENGLISH} "Add a Nesva shortcut to this desktop."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_SecMain)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_SecDesktop)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  SetShellVarContext current

  ; The logon autostart entries point at executables this uninstaller deletes,
  ; so they have to go as well: "Nesva" is the desktop window's optional tray
  ; entry, "NesvaAgent" the background agent's.
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Nesva"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "NesvaAgent"
  DeleteRegKey HKCU "Software\Nesva"
  DeleteRegKey HKCU "${UNINST_KEY}"

  Delete "$SMPROGRAMS\Nesva\Nesva.lnk"
  Delete "$SMPROGRAMS\Nesva\Uninstall Nesva.lnk"
  RMDir "$SMPROGRAMS\Nesva"
  Delete "$DESKTOP\Nesva.lnk"

  ; Binaries, the bundled Qt runtime and its plugin directories. User data is
  ; deliberately untouched: the profile database stays under
  ; %LOCALAPPDATA%\nesva and the archives live in whatever destinations the jobs
  ; were given; deleting either is not this uninstaller's business.
  RMDir /r "$INSTDIR\platforms"
  RMDir /r "$INSTDIR\sqldrivers"
  RMDir /r "$INSTDIR\tls"
  RMDir /r "$INSTDIR\imageformats"
  RMDir /r "$INSTDIR\iconengines"
  RMDir /r "$INSTDIR\styles"
  RMDir /r "$INSTDIR\generic"
  RMDir /r "$INSTDIR\networkinformation"
  Delete "$INSTDIR\nesva.exe"
  Delete "$INSTDIR\nesva-cli.exe"
  Delete "$INSTDIR\nesva-agent.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\qt.conf"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\README-windows.md"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
