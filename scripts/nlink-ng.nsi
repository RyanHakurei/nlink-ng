Unicode true
RequestExecutionLevel user

!include "MUI2.nsh"

!ifndef SOURCE
  !error "Pass /DSOURCE= the staged Windows folder"
!endif
!ifndef OUTFILE
  !error "Pass /DOUTFILE= the installer path"
!endif

Name "nlink-ng"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\nlink-ng"
InstallDirRegKey HKCU "Software\nlink-ng" "InstallDir"
BrandingText "nlink-ng 1.0.0"
ShowInstDetails show

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "nlink-ng 1.0.0"
!define MUI_WELCOMEPAGE_TEXT "This installs nlink-ng.$\r$\n$\r$\nDrag files and folders from the calculator onto your computer, or drop files onto the calculator to send them."
!define MUI_FINISHPAGE_RUN "$INSTDIR\n-link.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open nlink-ng"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "1.0.0.0"
VIAddVersionKey "ProductName" "nlink-ng"
VIAddVersionKey "FileVersion" "1.0.0"
VIAddVersionKey "ProductVersion" "1.0.0"
VIAddVersionKey "FileDescription" "nlink-ng installer"
VIAddVersionKey "LegalCopyright" "Ryan"

Section
  SetOutPath "$INSTDIR"
  File /r "${SOURCE}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\nlink-ng" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\nlink-ng" "DisplayName" "nlink-ng"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\nlink-ng" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\nlink-ng" "DisplayVersion" "1.0.0"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\nlink-ng" "Publisher" "Ryan"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\nlink-ng" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\nlink-ng" "NoRepair" 1
  CreateShortcut "$SMPROGRAMS\nlink-ng.lnk" "$INSTDIR\n-link.exe"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\nlink-ng.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\nlink-ng"
  DeleteRegKey HKCU "Software\nlink-ng"
SectionEnd
