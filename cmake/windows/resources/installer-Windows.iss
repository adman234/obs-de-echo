; Values are passed in by .github/scripts/Package-Windows.ps1
#ifndef AppName
  #error AppName, AppVersion, AppPublisher, AppURL, PluginName and SourceDir must be defined
#endif

[Setup]
AppId={{6C1C0B1E-7B0C-4F0B-9B0E-0D7E6A1D3E42}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginName}
DisableDirPage=yes
; Always install to the folder named after the plugin, even when upgrading
; an install made under the old name (obs-de-echo).
UsePreviousAppDir=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[InstallDelete]
; Remove the install from before the rename so OBS does not load two copies.
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\obs-de-echo"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
