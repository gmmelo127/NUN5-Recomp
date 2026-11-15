Unicode true
!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

Name "Naruto Shippuden: Ultimate Ninja 5 - Recomp"
OutFile "naruto5-recomp-setup-0.1-win64.exe"
InstallDir "$LOCALAPPDATA\Programs\Naruto5Recomp"
InstallDirRegKey HKCU "Software\Naruto5Recomp" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!define MUI_ABORTWARNING
!define MUI_WELCOMEFINISHPAGE_BITMAP "naruto5-recomp-toolchain-0.1-win64\icons\wizard.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "naruto5-recomp-toolchain-0.1-win64\icons\header.bmp"
!define MUI_HEADERIMAGE_RIGHT

!define MUI_ICON "naruto5-recomp-toolchain-0.1-win64\icons\naruto5.ico"
!define MUI_UNICON "naruto5-recomp-toolchain-0.1-win64\icons\naruto5.ico"
!define MUI_WELCOMEPAGE_TITLE "Naruto Shippuden: Ultimate Ninja 5"
!define MUI_WELCOMEPAGE_TEXT "Este instalador monta o recomp nativo a partir da sua ISO legal do jogo.$\r$\n$\r$\nNada e baixado: Zig, Python, SDL2 e wgpu-native ja estao no pacote. O build e feito na sua maquina."
!define MUI_FINISHPAGE_RUN "$INSTDIR\jogar.bat"
!define MUI_FINISHPAGE_RUN_TEXT "Jogar agora"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\LEIA-ME.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Abrir o LEIA-ME"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "naruto5-recomp-toolchain-0.1-win64\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
Page custom IsoPageCreate IsoPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "PortugueseBR"

Var IsoFile
Var IsoHandle
Var NtscCheck
Var NtscState
Var BuildCheck
Var BuildState

Function .onInit
  StrCpy $IsoFile ""
  StrCpy $NtscState ${BST_CHECKED}
  StrCpy $BuildState ${BST_CHECKED}
  ${GetParameters} $R0
  ${GetOptions} $R0 "/ISO=" $IsoFile
  ${GetOptions} $R0 "/NOBUILD" $R1
  ${IfNot} ${Errors}
    StrCpy $BuildState ${BST_UNCHECKED}
  ${EndIf}
FunctionEnd

Function IsoPageCreate
  !insertmacro MUI_HEADER_TEXT "Jogo" "Escolha a ISO do Naruto Shippuden: Ultimate Ninja 5"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 24u "Selecione a sua ISO (PAL SLES-556.05). A ISO nao acompanha este instalador."
  Pop $0
  ${NSD_CreateText} 0 30u 75% 14u "$IsoFile"
  Pop $IsoHandle
  ${NSD_CreateBrowseButton} 77% 30u 23% 14u "Procurar..."
  Pop $0
  ${NSD_OnClick} $0 BrowseIso
  ${NSD_CreateCheckbox} 0 55u 100% 14u "Aplicar patch NTSC/60 Hz (recomendado para a versao PAL)"
  Pop $NtscCheck
  ${NSD_SetState} $NtscCheck $NtscState
  ${NSD_CreateCheckbox} 0 75u 100% 14u "Compilar o jogo agora (leva varios minutos, mas deixa pronto para jogar)"
  Pop $BuildCheck
  ${NSD_SetState} $BuildCheck $BuildState
  nsDialogs::Show
FunctionEnd

Function IsoPageLeave
  ${NSD_GetText} $IsoHandle $IsoFile
  ${NSD_GetState} $NtscCheck $NtscState
  ${NSD_GetState} $BuildCheck $BuildState
  ${If} $IsoFile == ""
    MessageBox MB_ICONEXCLAMATION "Escolha a ISO do jogo."
    Abort
  ${EndIf}
  IfFileExists "$IsoFile" iso_ok
    MessageBox MB_ICONEXCLAMATION "ISO nao encontrada: $IsoFile"
    Abort
  iso_ok:
FunctionEnd

Function BrowseIso
  nsDialogs::SelectFileDialog open "$IsoFile" "ISO (*.iso;*.zip)|*.iso;*.zip|Todos os arquivos (*.*)|*.*"
  Pop $0
  ${If} $0 != ""
    ${NSD_SetText} $IsoHandle "$0"
  ${EndIf}
FunctionEnd

Section "Instalar" SecMain
  SetOutPath "$INSTDIR"
  File /r "naruto5-recomp-toolchain-0.1-win64\*"
  FileOpen $0 "$INSTDIR\iso_path.txt" w
  FileWrite $0 "$IsoFile$\r$\n"
  FileClose $0

  WriteRegStr HKCU "Software\Naruto5Recomp" "InstallDir" "$INSTDIR"
  CreateDirectory "$SMPROGRAMS\Naruto 5 Recomp"
  CreateShortCut "$SMPROGRAMS\Naruto 5 Recomp\Naruto Shippuden Ultimate Ninja 5.lnk" "$INSTDIR\jogar.bat" "" "$INSTDIR\icons\naruto5.ico" 0
  CreateShortCut "$DESKTOP\Naruto Shippuden Ultimate Ninja 5.lnk" "$INSTDIR\jogar.bat" "" "$INSTDIR\icons\naruto5.ico" 0
  CreateShortCut "$SMPROGRAMS\Naruto 5 Recomp\Reconfigurar ou reinstalar.lnk" "$INSTDIR\instalar.bat"
  CreateShortCut "$SMPROGRAMS\Naruto 5 Recomp\Diagnostico.lnk" "$INSTDIR\diagnostico.bat"
  CreateShortCut "$SMPROGRAMS\Naruto 5 Recomp\Diagnostico de janela.lnk" "$INSTDIR\diagnostico_janela.bat"
  CreateShortCut "$SMPROGRAMS\Naruto 5 Recomp\Otimizar VU1.lnk" "$INSTDIR\otimizar_vu.bat"
  CreateShortCut "$SMPROGRAMS\Naruto 5 Recomp\Jogar (VSync FIFO).lnk" "$INSTDIR\jogar_fifo.bat"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Naruto5Recomp" "DisplayName" "Naruto Shippuden: Ultimate Ninja 5 - Recomp"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Naruto5Recomp" "DisplayIcon" "$INSTDIR\icons\naruto5.ico"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Naruto5Recomp" "DisplayVersion" "0.1"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Naruto5Recomp" "Publisher" "Naruto 5 Recomp"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Naruto5Recomp" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Naruto5Recomp" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Naruto5Recomp" "NoRepair" 1

  ${If} $BuildState == ${BST_CHECKED}
    DetailPrint "Gerando o build do jogo (pode levar varios minutos)..."
    StrCpy $R0 '--frames 12000 --rounds 4'
    ${If} $NtscState == ${BST_CHECKED}
      StrCpy $R1 '--pnach "$INSTDIR\patches\PALtoNTSC_fixed.pnach" --pnach-section "NTSC Video Mode"'
    ${Else}
      StrCpy $R1 ''
    ${EndIf}
    nsExec::ExecToLog '"$INSTDIR\toolchain\python\python.exe" "$INSTDIR\scripts\port_win.py" naruto "$IsoFile" $R0 $R1'
    Pop $R2
    ${If} $R2 != 0
      MessageBox MB_ICONSTOP "O build falhou. Veja o detalhe do instalador e o install.log. Use instalar.bat para tentar de novo."
    ${EndIf}
  ${EndIf}
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\Naruto Shippuden Ultimate Ninja 5.lnk"
  RMDir /r "$SMPROGRAMS\Naruto 5 Recomp"
  DeleteRegKey HKCU "Software\Naruto5Recomp"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Naruto5Recomp"
  MessageBox MB_YESNO "Remover tambem saves e builds em $INSTDIR\games?" IDNO keep
  RMDir /r "$INSTDIR\games"
keep:
  RMDir /r "$INSTDIR"
SectionEnd
