#define Version Trim(FileRead(FileOpen("..\VERSION")))
#define ProjectName GetEnv('PROJECT_NAME')
#define ProductName GetEnv('PRODUCT_NAME')
#define Publisher GetEnv('COMPANY_NAME')
#define TargetArch GetEnv('GUITARFX_WINDOWS_ARCH')
; build_windows.bat exports the resolved architecture; keep x64 as a fallback for standalone packaging outside that script.
#if TargetArch == ""
  #define TargetArch "x64"
#endif
#if TargetArch == "Win32"
  #define CommonFiles "{commoncf32}"
  #define ProgramFiles "{commonpf32}"
#else
  ; 64-bit targets (x64 and ARM64) use the native 64-bit Program Files and Common Files locations.
  #define CommonFiles "{commoncf64}"
  #define ProgramFiles "{commonpf64}"
#endif
#define Year GetDateTimeString("yyyy","","")
#define WebView2RuntimeUrl "https://go.microsoft.com/fwlink/p/?LinkId=2124703"

; 'Types': What get displayed during the setup
[Types]
Name: "full"; Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

; Components are used inside the script and can be composed of a set of 'Types'
[Components]
Name: "standalone"; Description: "Standalone application"; Types: full custom
Name: "vst3"; Description: "VST3 plugin"; Types: full custom
Name: "clap"; Description: "CLAP plugin"; Types: custom; Flags: checkablealone

[Setup]
#if TargetArch == "Win32"
ArchitecturesAllowed=x86compatible
#else
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
#endif
AppName={#ProductName}
OutputBaseFilename={#ProductName}-{#Version}-Windows
AppCopyright=Copyright (C) {#Year} {#Publisher}
AppPublisher={#Publisher}
AppVersion={#Version}
DefaultDirName="{#CommonFiles}\VST3\{#ProductName}.vst3"
DisableDirPage=yes

; MAKE SURE YOU READ/MODIFY THE EULA BEFORE USING IT
LicenseFile="resources\EULA"
UninstallFilesDir="{commonappdata}\{#ProductName}\uninstall"

[UninstallDelete]
Type: filesandordirs; Name: "{#ProgramFiles}\{#Publisher}\{#ProductName}"
Type: filesandordirs; Name: "{#CommonFiles}\VST3\{#ProductName}.vst3"
Type: filesandordirs; Name: "{#CommonFiles}\CLAP\{#ProductName}.clap"

; MSVC adds a .ilk when building the plugin. Let's not include that.
[Files]
Source: "..\Builds\{#ProjectName}_artefacts\Release\VST3\{#ProductName}.vst3\*"; DestDir: "{#CommonFiles}\VST3\{#ProductName}.vst3\"; Excludes: *.ilk,node_modules\*,*\node_modules\*,ts\*,*\ts\*,Testing\*,*\Testing\*,tests\*,*\tests\*,assets\amps\*,assets\ir\*; Flags: ignoreversion recursesubdirs; Components: vst3
Source: "..\Builds\{#ProjectName}_artefacts\Release\CLAP\{#ProductName}.clap"; DestDir: "{#CommonFiles}\CLAP\"; Flags: ignoreversion; Components: clap
Source: "..\Builds\{#ProjectName}_artefacts\Release\CLAP\resources\*"; DestDir: "{#CommonFiles}\CLAP\resources\"; Excludes: node_modules\*,*\node_modules\*,ts\*,*\ts\*,Testing\*,*\Testing\*,tests\*,*\tests\*,assets\amps\*,assets\ir\*; Flags: ignoreversion recursesubdirs createallsubdirs; Components: clap
Source: "..\Builds\{#ProjectName}_artefacts\Release\Standalone\*"; DestDir: "{#ProgramFiles}\{#Publisher}\{#ProductName}"; Excludes: *.ilk,node_modules\*,*\node_modules\*,ts\*,*\ts\*,Testing\*,*\Testing\*,tests\*,*\tests\*,assets\amps\*,assets\ir\*; Flags: ignoreversion recursesubdirs; Components: standalone


[Icons]
Name: "{autoprograms}\{#ProductName}"; Filename: "{#ProgramFiles}\{#Publisher}\{#ProductName}\{#ProductName}.exe"; Components: standalone
Name: "{autoprograms}\Uninstall {#ProductName}"; Filename: "{uninstallexe}"

; This is optional, for preset or other plugin data
[Run]
Filename: "{#ProgramFiles}\{#Publisher}\{#ProductName}\{#ProductName}.exe"; \
    Description: "Launch {#ProductName}"; \
    Flags: nowait postinstall skipifsilent; Components: standalone

[Code]
function IsWebView2RuntimeInstalled: Boolean;
var
    Version: string;
begin
    Result :=
        (RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) and (Version <> ''))
        or
        (RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) and (Version <> ''))
        or
        (RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) and (Version <> ''));
end;

function NeedsWebView2Runtime: Boolean;
begin
    Result := not IsWebView2RuntimeInstalled;
end;

function InstallWebView2Runtime: Boolean;
var
    InstallerPath: string;
    DownloadCommand: string;
    ResultCode: Integer;
begin
    Result := True;

    if not NeedsWebView2Runtime then
        Exit;

    InstallerPath := ExpandConstant('{tmp}\MicrosoftEdgeWebView2Setup.exe');
    DownloadCommand := '/C powershell -NoProfile -ExecutionPolicy Bypass -Command "' +
                       '$ProgressPreference = ''SilentlyContinue''; ' +
                       'Invoke-WebRequest -UseBasicParsing -Uri ''{#WebView2RuntimeUrl}'' -OutFile ''' + InstallerPath + '''"';

    if not Exec(ExpandConstant('{cmd}'), DownloadCommand, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
    begin
        Log('Failed to download WebView2 runtime bootstrapper. ExitCode=' + IntToStr(ResultCode));
        Result := False;
        Exit;
    end;

    if not FileExists(InstallerPath) then
    begin
        Log('WebView2 runtime bootstrapper was not downloaded: ' + InstallerPath);
        Result := False;
        Exit;
    end;

    if not Exec(InstallerPath, '/silent /install', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
    begin
        Log('WebView2 runtime installer failed. ExitCode=' + IntToStr(ResultCode));
        Result := False;
        Exit;
    end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
    SharedUiPath: string;
begin
    if CurStep <> ssPostInstall then
        Exit;

    if WizardIsComponentSelected('standalone') then
    begin
        if not InstallWebView2Runtime then
            Log('Continuing install without WebView2 runtime; standalone UI may not load until runtime is installed.');
    end;

    SharedUiPath := ExpandConstant('{commonappdata}\{#ProductName}\resources\ui');

end;
