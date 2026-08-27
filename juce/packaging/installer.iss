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
Name: "standalone"; Description: "Standalone application"; Types: full custom; Flags: checkablealone
Name: "vst3"; Description: "VST3 plugin"; Types: full custom; Flags: checkablealone
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
SetupIconFile="..\Builds\{#ProjectName}_artefacts\JuceLibraryCode\icon.ico"
UninstallDisplayIcon="{code:GetStandaloneExePath}"
AppCopyright=Copyright (C) {#Year} {#Publisher}
AppPublisher={#Publisher}
AppVersion={#Version}
VersionInfoVersion={#Version}
DefaultDirName="{#CommonFiles}\VST3\{#ProductName}.vst3"
DisableDirPage=yes

; MAKE SURE YOU READ/MODIFY THE EULA BEFORE USING IT
LicenseFile="resources\EULA"
UninstallFilesDir="{commonappdata}\{#ProductName}\uninstall"

[UninstallDelete]
; Inno Setup already removes every file it installed via the [Files] section.
Type: filesandordirs; Name: "{code:GetVst3Dir}"
Type: filesandordirs; Name: "{code:GetClapBinaryPath}"

[Registry]
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: string; ValueName: "StandaloneDir"; ValueData: "{code:GetStandaloneDir}"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: string; ValueName: "VST3Dir"; ValueData: "{code:GetVst3Dir}"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: string; ValueName: "CLAPDir"; ValueData: "{code:GetClapDir}"; Flags: uninsdeletekeyifempty

; MSVC adds a .ilk when building the plugin. Let's not include that.
[Files]
Source: "..\Builds\{#ProjectName}_artefacts\Release\VST3\{#ProductName}.vst3\*"; DestDir: "{code:GetVst3Dir}"; Excludes: *.ilk,node_modules\*,*\node_modules\*,ts\*,*\ts\*,Testing\*,*\Testing\*,tests\*,*\tests\*,assets\amps\*,assets\ir\*; Flags: ignoreversion recursesubdirs; Components: vst3
Source: "..\Builds\{#ProjectName}_artefacts\Release\CLAP\{#ProductName}.clap"; DestDir: "{code:GetClapDir}"; Flags: ignoreversion; Components: clap
Source: "..\Builds\{#ProjectName}_artefacts\Release\CLAP\resources\*"; DestDir: "{code:GetClapResourcesDir}"; Excludes: node_modules\*,*\node_modules\*,ts\*,*\ts\*,Testing\*,*\Testing\*,tests\*,*\tests\*,assets\amps\*,assets\ir\*; Flags: ignoreversion recursesubdirs createallsubdirs; Components: clap
Source: "..\Builds\{#ProjectName}_artefacts\Release\Standalone\*"; DestDir: "{code:GetStandaloneDir}"; Excludes: *.ilk,node_modules\*,*\node_modules\*,ts\*,*\ts\*,Testing\*,*\Testing\*,tests\*,*\tests\*,assets\amps\*,assets\ir\*; Flags: ignoreversion recursesubdirs; Components: standalone


[Icons]
Name: "{autoprograms}\{#ProductName}"; Filename: "{code:GetStandaloneExePath}"; Components: standalone
Name: "{autoprograms}\Uninstall {#ProductName}"; Filename: "{uninstallexe}"

; This is optional, for preset or other plugin data
[Run]
Filename: "{code:GetStandaloneExePath}"; \
    Description: "Launch {#ProductName}"; \
    Flags: nowait postinstall skipifsilent; Components: standalone

[Code]
const
    InstallPathsRegKey = 'Software\{#Publisher}\{#ProductName}\Installer';

var
    InstallPathsPage: TInputDirWizardPage;

function TrimmedPath(const Path: string): string;
begin
    Result := RemoveBackslashUnlessRoot(Trim(Path));
end;

function ReadSavedInstallPath(const ValueName: string; const Fallback: string): string;
begin
    Result := '';

    if not RegQueryStringValue(HKLM, InstallPathsRegKey, ValueName, Result) then
        if not RegQueryStringValue(HKCU, InstallPathsRegKey, ValueName, Result) then
            Result := Fallback;

    Result := TrimmedPath(Result);
    if Result = '' then
        Result := TrimmedPath(Fallback);
end;

function GetStandaloneDir(Param: string): string;
begin
    if Assigned(InstallPathsPage) and (Trim(InstallPathsPage.Values[0]) <> '') then
        Result := TrimmedPath(InstallPathsPage.Values[0])
    else
        Result := ReadSavedInstallPath('StandaloneDir', ExpandConstant('{#ProgramFiles}\{#Publisher}\{#ProductName}'));
end;

function GetVst3Dir(Param: string): string;
begin
    if Assigned(InstallPathsPage) and (Trim(InstallPathsPage.Values[1]) <> '') then
        Result := TrimmedPath(InstallPathsPage.Values[1])
    else
        Result := ReadSavedInstallPath('VST3Dir', ExpandConstant('{#CommonFiles}\VST3\{#ProductName}.vst3'));
end;

function GetClapDir(Param: string): string;
begin
    if Assigned(InstallPathsPage) and (Trim(InstallPathsPage.Values[2]) <> '') then
        Result := TrimmedPath(InstallPathsPage.Values[2])
    else
        Result := ReadSavedInstallPath('CLAPDir', ExpandConstant('{#CommonFiles}\CLAP'));
end;

function GetClapResourcesDir(Param: string): string;
begin
    Result := AddBackslash(GetClapDir('')) + 'resources';
end;

function GetClapBinaryPath(Param: string): string;
begin
    Result := AddBackslash(GetClapDir('')) + '{#ProductName}.clap';
end;

function GetStandaloneExePath(Param: string): string;
begin
    Result := AddBackslash(GetStandaloneDir('')) + '{#ProductName}.exe';
end;

procedure InitializeWizard;
begin
    InstallPathsPage := CreateInputDirPage(
        wpSelectComponents,
        'Install locations',
        'Optional custom install paths',
        'Choose where each selected component will be installed. Leave defaults if you do not need custom paths.',
        False,
        ''
    );

    InstallPathsPage.Add('Standalone application folder:');
    InstallPathsPage.Values[0] := GetStandaloneDir('');

    InstallPathsPage.Add('VST3 bundle folder (.vst3):');
    InstallPathsPage.Values[1] := GetVst3Dir('');

    InstallPathsPage.Add('CLAP plugin folder:');
    InstallPathsPage.Values[2] := GetClapDir('');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
    Result := False;

    if (PageID = InstallPathsPage.ID) and (WizardSetupType(False) <> 'custom') then
        Result := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
    Result := True;

    if CurPageID = wpSelectComponents then
    begin
        if not WizardIsComponentSelected('standalone')
            and not WizardIsComponentSelected('vst3')
            and not WizardIsComponentSelected('clap') then
        begin
            MsgBox('Select at least one component to install.', mbError, MB_OK);
            Result := False;
        end;

        Exit;
    end;

    if CurPageID <> InstallPathsPage.ID then
        Exit;

    if WizardIsComponentSelected('standalone') and (Trim(GetStandaloneDir('')) = '') then
    begin
        MsgBox('Standalone install folder cannot be empty.', mbError, MB_OK);
        Result := False;
        Exit;
    end;

    if WizardIsComponentSelected('vst3') and (Trim(GetVst3Dir('')) = '') then
    begin
        MsgBox('VST3 install folder cannot be empty.', mbError, MB_OK);
        Result := False;
        Exit;
    end;

    if WizardIsComponentSelected('clap') and (Trim(GetClapDir('')) = '') then
    begin
        MsgBox('CLAP install folder cannot be empty.', mbError, MB_OK);
        Result := False;
        Exit;
    end;
end;

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
