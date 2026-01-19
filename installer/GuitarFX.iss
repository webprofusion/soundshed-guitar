; Inno Setup script for GuitarFX
; Build outputs expected:
;  - VST3 bundle: src\build\SoundshedGuitar.vst3
;  - Standalone app: src\build\src\platform\app\Release\SoundshedGuitar.exe
;  - Standalone resources: src\build\src\platform\app\Release\resources\

#define AppName "Soundshed Guitar"
#define AppPublisher "Soundshed"
#define AppVersion "0.4.0"
#define AppURL "https://soundshed.com"
#define AppExeName "SoundshedGuitar.exe"
#define Vst3BundleName "SoundshedGuitar.vst3"

[Setup]
AppId={{FBA4B6C7-3B1E-4B6C-9AE6-78B6F2E1F2F1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
DefaultDirName={pf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes
OutputDir=dist
OutputBaseFilename={#AppName}-Setup-{#AppVersion}
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayIcon={app}\{#AppExeName}

[Types]
Name: "full"; Description: "Full installation"; Flags: iscustom
Name: "vst3only"; Description: "VST3 only"
Name: "standaloneonly"; Description: "Standalone only"

[Components]
Name: "vst3"; Description: "VST3 Plugin"; Types: full vst3only
Name: "standalone"; Description: "Standalone Application"; Types: full standaloneonly

[Tasks]
Name: "desktopicon"; Description: "Create a Desktop icon"; GroupDescription: "Additional icons:"; Components: standalone
Name: "startmenu"; Description: "Create a Start Menu shortcut"; GroupDescription: "Additional icons:"; Components: standalone; Flags: checkedonce

[Files]
; VST3 bundle
Source: "..\src\build\{#Vst3BundleName}\*"; DestDir: "{commoncf}\VST3\{#Vst3BundleName}"; Components: vst3; Flags: recursesubdirs createallsubdirs

; Standalone app + resources
Source: "..\src\build\src\platform\app\Release\{#AppExeName}"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "..\src\build\src\platform\app\Release\resources\*"; DestDir: "{app}\resources"; Components: standalone; Flags: recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Components: standalone; Tasks: startmenu
Name: "{commondesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Components: standalone; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{app}\resources"
