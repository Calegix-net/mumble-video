; NSIS installer for the Windows client.
;
; This exists alongside the MSI because wixl, the only WiX implementation that runs on Linux, cannot
; emit dialogs at all: the MSI it produces installs correctly but shows no wizard, which reads to
; anyone double-clicking it as the installer having done nothing. NSIS builds a real wizard from
; Linux, so this is what a person should download.
;
; The MSI is kept for deployment tooling (Group Policy, Intune, msiexec /qn), which wants an MSI and
; does not want a wizard.
;
; Built by scripts/build-nsis.sh, which passes BUNDLE_DIR and VERSION in.

!include "MUI2.nsh"
!include "x64.nsh"

Name "Mumble Video"
OutFile "${OUT_FILE}"
Unicode true

; Per-machine into Program Files, so the installer requests elevation up front rather than failing
; on the first file it cannot write.
InstallDir "$PROGRAMFILES64\Mumble Video"
RequestExecutionLevel admin

!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

; Offer to launch on finish, which is the one thing people actually want from an installer's last page.
!define MUI_FINISHPAGE_RUN "$INSTDIR\mumble.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Mumble Video"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Mumble Video" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  ; The whole bundle, subdirectories included: Qt's platform and multimedia plugins live in
  ; directories beside the executable and are loaded by name, so a flat copy would start a client
  ; that cannot open a window or find a camera.
  File /r "${BUNDLE_DIR}\*.*"

  CreateDirectory "$SMPROGRAMS\Mumble Video"
  CreateShortcut "$SMPROGRAMS\Mumble Video\Mumble Video.lnk" "$INSTDIR\mumble.exe"
  CreateShortcut "$SMPROGRAMS\Mumble Video\Uninstall.lnk" "$INSTDIR\uninstall.exe"

  ; Registered so the app appears in Settings > Apps and can be removed like anything else.
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MumbleVideo" \
      "DisplayName" "Mumble Video"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MumbleVideo" \
      "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MumbleVideo" \
      "Publisher" "the mumble-video project"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MumbleVideo" \
      "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MumbleVideo" \
      "DisplayIcon" "$\"$INSTDIR\mumble.exe$\""
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MumbleVideo" \
      "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MumbleVideo" \
      "NoRepair" 1

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  ; RMDir /r on the install directory only, never on a parent: $INSTDIR is user-editable on the
  ; directory page and a careless recursive delete higher up would take other things with it.
  Delete "$SMPROGRAMS\Mumble Video\Mumble Video.lnk"
  Delete "$SMPROGRAMS\Mumble Video\Uninstall.lnk"
  RMDir "$SMPROGRAMS\Mumble Video"

  RMDir /r "$INSTDIR"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MumbleVideo"
SectionEnd
