#define Version Trim(FileRead(FileOpen("..\VERSION")))
#define ProjectName GetEnv('PROJECT_NAME')
#define ProductName GetEnv('PRODUCT_NAME')
#define Publisher GetEnv('COMPANY_NAME')
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
; Name: "clap"; Description: "CLAP plugin"; Types: full custom

[Setup]
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
AppName={#ProductName}
OutputBaseFilename={#ProductName}-{#Version}-Windows
AppCopyright=Copyright (C) {#Year} {#Publisher}
AppPublisher={#Publisher}
AppVersion={#Version}
DefaultDirName="{commoncf64}\VST3\{#ProductName}.vst3"
DisableDirPage=yes

; MAKE SURE YOU READ/MODIFY THE EULA BEFORE USING IT
LicenseFile="resources\EULA"
UninstallFilesDir="{commonappdata}\{#ProductName}\uninstall"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\{#ProductName}Data"
Type: filesandordirs; Name: "{commonpf64}\{#Publisher}\{#ProductName}\resources\ui"
Type: filesandordirs; Name: "{commoncf64}\VST3\{#ProductName}.vst3\Contents\x86_64-win\resources\ui"
Type: filesandordirs; Name: "{commonappdata}\{#ProductName}\resources"

; MSVC adds a .ilk when building the plugin. Let's not include that.
[Files]
Source: "..\Builds\{#ProjectName}_artefacts\Release\VST3\{#ProductName}.vst3\*"; DestDir: "{commoncf64}\VST3\{#ProductName}.vst3\"; Excludes: *.ilk,node_modules; Flags: ignoreversion recursesubdirs; Components: vst3
; Source: "..\Builds\{#ProjectName}_artefacts\Release\CLAP\{#ProductName}.clap"; DestDir: "{commoncf64}\CLAP\"; Flags: ignoreversion; Components: clap
Source: "..\Builds\{#ProjectName}_artefacts\Release\Standalone\*"; DestDir: "{commonpf64}\{#Publisher}\{#ProductName}";  Flags: ignoreversion recursesubdirs; Components: standalone
Source: "..\..\core\ui\*"; DestDir: "{commonappdata}\{#ProductName}\resources\ui";  Excludes: node_modules; Flags: ignoreversion recursesubdirs createallsubdirs; Components: standalone vst3

[Icons]
Name: "{autoprograms}\{#ProductName}"; Filename: "{commonpf64}\{#Publisher}\{#ProductName}\{#ProductName}.exe"; Components: standalone
Name: "{autoprograms}\Uninstall {#ProductName}"; Filename: "{uninstallexe}"

; This is optional, for preset or other plugin data
[Run]
Filename: "{cmd}"; \
    WorkingDir: "{commoncf64}\VST3"; \
    Parameters: "/C mklink /D ""{commoncf64}\VST3\{#ProductName}Data"" ""{commonappdata}\{#ProductName}"""; \
    Flags: runascurrentuser; Components: vst3

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

function CreateDirectorySymlink(const LinkPath: string; const TargetPath: string): Boolean;
var
    ResultCode: Integer;
begin
    if not DirExists(TargetPath) then
    begin
        Log('Symlink target does not exist: ' + TargetPath);
        Result := False;
        Exit;
    end;

    if DirExists(LinkPath) then
    begin
        DelTree(LinkPath, True, True, True);
    end;

    if not DirExists(ExtractFileDir(LinkPath)) then
    begin
        if not ForceDirectories(ExtractFileDir(LinkPath)) then
        begin
            Log('Failed to create parent directory for: ' + LinkPath);
            Result := False;
            Exit;
        end;
    end;

    Result := Exec(ExpandConstant('{cmd}'),
                   '/C mklink /D "' + LinkPath + '" "' + TargetPath + '"',
                   '',
                   SW_HIDE,
                   ewWaitUntilTerminated,
                   ResultCode) and (ResultCode = 0);

    if not Result then
        Log('Failed to create symlink. Link=' + LinkPath + ' Target=' + TargetPath + ' ExitCode=' + IntToStr(ResultCode));
end;

function CopyDirectoryTree(const SourcePath: string; const DestPath: string): Boolean;
var
    ResultCode: Integer;
begin
    if not DirExists(SourcePath) then
    begin
        Log('Copy source does not exist: ' + SourcePath);
        Result := False;
        Exit;
    end;

    if DirExists(DestPath) then
    begin
        DelTree(DestPath, True, True, True);
    end;

    if not DirExists(ExtractFileDir(DestPath)) then
    begin
        if not ForceDirectories(ExtractFileDir(DestPath)) then
        begin
            Log('Failed to create copy destination parent: ' + ExtractFileDir(DestPath));
            Result := False;
            Exit;
        end;
    end;

    Result := Exec(ExpandConstant('{cmd}'),
                   '/C xcopy /E /I /Y "' + SourcePath + '" "' + DestPath + '"',
                   '',
                   SW_HIDE,
                   ewWaitUntilTerminated,
                   ResultCode) and (ResultCode = 0);

    if not Result then
        Log('Fallback copy failed. Source=' + SourcePath + ' Dest=' + DestPath + ' ExitCode=' + IntToStr(ResultCode));
end;

procedure EnsureUiAtPath(const LinkPath: string; const SharedUiPath: string);
begin
    if CreateDirectorySymlink(LinkPath, SharedUiPath) then
        Exit;

    Log('Falling back to physical UI copy at: ' + LinkPath);

    if not CopyDirectoryTree(SharedUiPath, LinkPath) then
        Log('UI fallback copy was not successful for: ' + LinkPath);
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

    if WizardIsComponentSelected('standalone') then
    begin
        EnsureUiAtPath(
            ExpandConstant('{commonpf64}\{#Publisher}\{#ProductName}\resources\ui'),
            SharedUiPath);
    end;

    if WizardIsComponentSelected('vst3') then
    begin
        EnsureUiAtPath(
            ExpandConstant('{commoncf64}\VST3\{#ProductName}.vst3\Contents\x86_64-win\resources\ui'),
            SharedUiPath);
    end;
end;
