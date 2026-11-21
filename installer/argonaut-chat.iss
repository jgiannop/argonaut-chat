; The Windows installer (Inno Setup): the folder the build puts together
; (argonaut-chat.exe, Qt's libraries, libsodium, SQLCipher, QtKeychain and
; Microsoft's C++ runtime) into Program Files, with a Start menu entry and an
; uninstaller.
;
;   iscc /DAppVersion=0.2.0 /DSource=..\dist installer\argonaut-chat.iss
;
; Without administrator rights it installs for the current user only.
; Uninstalling leaves the history (%APPDATA%\Argonaut\argonaut-chat) and its
; key (in Windows Credential Manager, "Argonaut Chat") in place.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Source
  #define Source "..\dist"
#endif

[Setup]
AppId={{54850657-F0B1-4E6C-9327-8C95BFD0C087}
AppName=Argonaut Chat
AppVersion={#AppVersion}
AppPublisher=johngiannop
AppPublisherURL=https://github.com/jgiannop/argonaut-chat
AppSupportURL=https://github.com/jgiannop/argonaut-chat/issues
DefaultDirName={autopf}\Argonaut Chat
DefaultGroupName=Argonaut Chat
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\data\argonaut-chat.ico
UninstallDisplayIcon={app}\argonaut-chat.exe
OutputDir=..\installer-out
OutputBaseFilename=argonaut-chat-{#AppVersion}-windows-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; It may be running in the tray: Setup asks to close it first.
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#Source}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Argonaut Chat"; Filename: "{app}\argonaut-chat.exe"
Name: "{autodesktop}\Argonaut Chat"; Filename: "{app}\argonaut-chat.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\argonaut-chat.exe"; Description: "{cm:LaunchProgram,Argonaut Chat}"; Flags: nowait postinstall skipifsilent
