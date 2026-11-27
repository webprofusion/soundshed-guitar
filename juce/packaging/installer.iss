#define Version Trim(FileRead(FileOpen("..\VERSION")))
#define ProjectName GetEnv('PROJECT_NAME')
#define ProductName GetEnv('PRODUCT_NAME')
#define Publisher GetEnv('COMPANY_NAME')
#define TargetArch GetEnv('GUITARFX_WINDOWS_ARCH')
#define BuildDir GetEnv('GUITARFX_WINDOWS_BUILD_DIR')
#define Arm64EcBuildDir GetEnv('GUITARFX_WINDOWS_ARM64EC_BUILD_DIR')
; build-installer.bat sets this to 1 when the AAX is signed (or GUITARFX_ALLOW_UNSIGNED_AAX=1).
#define IncludeAAX GetEnv('GUITARFX_INCLUDE_AAX')
#if BuildDir == ""
  #define BuildDir "..\Builds"
#endif
#if Arm64EcBuildDir == ""
  #define Arm64EcBuildDir "..\builds-arm64ec"
#endif
#define ArtefactsDir BuildDir + "\" + ProjectName + "_artefacts\Release"
#define Arm64EcArtefactsDir Arm64EcBuildDir + "\" + ProjectName + "_artefacts\Release"
#define UiStageDir BuildDir + "\ui-stage"
; build_windows.bat exports the resolved architecture; keep x64 as a fallback for standalone packaging outside that script.
#if TargetArch == ""
  #define TargetArch "x64"
#endif
; JUCE builds AAX for x64 only.
#if IncludeAAX != "1" || TargetArch != "x64"
  #define IncludeAAX "0"
#endif
#if TargetArch == "Win32"
  #define CommonFiles "{commoncf32}"
  #define ProgramFiles "{commonpf32}"
#else
  ; 64-bit targets (x64 and ARM64) use the native 64-bit Program Files and Common Files locations.
  #define CommonFiles "{commoncf64}"
  #define ProgramFiles "{commonpf64}"
#endif
; The VST3 bundle's folder for this architecture's library.
#if TargetArch == "Win32"
  #define Vst3ArchDir "x86-win"
#elif TargetArch == "ARM64"
  #define Vst3ArchDir "arm64-win"
#else
  #define Vst3ArchDir "x86_64-win"
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
#if IncludeAAX == "1"
Name: "aax"; Description: "AAX plugin for Pro Tools"; Types: full custom; Flags: checkablealone
#endif

; Advanced choice, off unless ticked (or passed: /TASKS="nativeuionly" for a silent install). Inno
; remembers it for the next install over this one. Ticked, the WebView2 runtime is not installed:
; the native UI draws without a browser.
[Tasks]
Name: "nativeuionly"; Description: "Use only the native (Nano) UI (beta). WebView2 is not installed."; GroupDescription: "Advanced:"; Flags: unchecked

[Setup]
#if TargetArch == "Win32"
ArchitecturesAllowed=x86compatible
#else
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
#endif
AppName={#ProductName}
OutputBaseFilename={#ProductName}-{#Version}-Windows
SetupIconFile="{#BuildDir}\{#ProjectName}_artefacts\JuceLibraryCode\icon.ico"
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
#if IncludeAAX == "1"
Type: filesandordirs; Name: "{code:GetAaxDir}"
#endif
#if TargetArch == "ARM64"
Type: filesandordirs; Name: "{code:GetClapDir}\{#ProductName} (ARM64EC).clap"
#endif
; The editor's WebView2 profile (cache and storage; see juce/source/WebView2UserData.h),
; plus the per-launch profiles builds before September 2026 left under %TEMP%.
Type: filesandordirs; Name: "{localappdata}\Soundshed Guitar\WebView2*"
Type: filesandordirs; Name: "{%TEMP}\SoundshedGuitarWebView2"
Type: filesandordirs; Name: "{%TEMP}\SoundshedGuitarWebView2Check"

[Registry]
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: string; ValueName: "StandaloneDir"; ValueData: "{code:GetStandaloneDir}"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: string; ValueName: "VST3Dir"; ValueData: "{code:GetVst3Dir}"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: string; ValueName: "CLAPDir"; ValueData: "{code:GetClapDir}"; Flags: uninsdeletekeyifempty
#if IncludeAAX == "1"
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: string; ValueName: "AAXDir"; ValueData: "{code:GetAaxDir}"; Flags: uninsdeletekeyifempty
#endif
; Read by the app and the plugins (PluginProcessorAdapter::NativeEditorOnly): 1 builds only the
; native UI and hides the way back to the WebView one. Written either way, so unticking it in a
; later install undoes it.
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: dword; ValueName: "NativeUiOnly"; ValueData: "1"; Tasks: nativeuionly; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\{#Publisher}\{#ProductName}\Installer"; ValueType: dword; ValueName: "NativeUiOnly"; ValueData: "0"; Tasks: not nativeuionly; Flags: uninsdeletevalue

; MSVC adds a .ilk when building the plugin. Let's not include that.
[Files]
Source: "{#ArtefactsDir}\VST3\{#ProductName}.vst3\*"; DestDir: "{code:GetVst3Dir}"; Excludes: *.ilk,resources\*,*\resources\*; Flags: ignoreversion recursesubdirs; Components: vst3
Source: "{#ArtefactsDir}\CLAP\{#ProductName}.clap"; DestDir: "{code:GetClapDir}"; Flags: ignoreversion; Components: clap
Source: "{#ArtefactsDir}\Standalone\*"; DestDir: "{code:GetStandaloneDir}"; Excludes: *.ilk,resources\*,*\resources\*; Flags: ignoreversion recursesubdirs; Components: standalone
; The native (beta) UI's fonts (juce/cmake/NanoQUi.cmake; skipifsourcedoesntexist for a
; build without it). Elements finds them only beside the binary that draws: resources\ next to the
; exe or the VST3's library, and "<name> Resources" next to the CLAP, which keeps them out of the
; resources folder every vendor's CLAP shares.
Source: "{#ArtefactsDir}\Standalone\resources\*.ttf"; DestDir: "{code:GetStandaloneDir}\resources"; Flags: ignoreversion skipifsourcedoesntexist; Components: standalone
Source: "{#ArtefactsDir}\VST3\{#ProductName}.vst3\Contents\{#Vst3ArchDir}\resources\*.ttf"; DestDir: "{code:GetVst3Dir}\Contents\{#Vst3ArchDir}\resources"; Flags: ignoreversion skipifsourcedoesntexist; Components: vst3
Source: "{#ArtefactsDir}\CLAP\resources\*.ttf"; DestDir: "{code:GetClapDir}\{#ProductName} Resources"; Flags: ignoreversion skipifsourcedoesntexist; Components: clap
#if IncludeAAX == "1"
; The AAX bundle, laid out like the VST3: the library under Contents\x64, its fonts beside it.
Source: "{#ArtefactsDir}\AAX\{#ProductName}.aaxplugin\*"; DestDir: "{code:GetAaxDir}"; Excludes: *.ilk,resources\*,*\resources\*; Flags: ignoreversion recursesubdirs; Components: aax
Source: "{#ArtefactsDir}\AAX\{#ProductName}.aaxplugin\Contents\x64\resources\*.ttf"; DestDir: "{code:GetAaxDir}\Contents\x64\resources"; Flags: ignoreversion skipifsourcedoesntexist; Components: aax
Source: "{#UiStageDir}\*"; DestDir: "{commonappdata}\{#ProductName}\resources\ui"; Excludes: assets\amps\*,*\assets\amps\*,assets\ir\*,*\assets\ir\*; Flags: ignoreversion recursesubdirs createallsubdirs; Components: standalone or vst3 or clap or aax
#else
Source: "{#UiStageDir}\*"; DestDir: "{commonappdata}\{#ProductName}\resources\ui"; Excludes: assets\amps\*,*\assets\amps\*,assets\ir\*,*\assets\ir\*; Flags: ignoreversion recursesubdirs createallsubdirs; Components: standalone or vst3 or clap
#endif
#if TargetArch == "ARM64"
; The ARM64EC plugins, beside the native ARM64 ones above. A host loads the one built for its own
; kind of process: REAPER's Windows ARM build is ARM64EC and cannot load an ARM64 plugin, and a
; native ARM64 host cannot load an ARM64EC one. A VST3 bundle has a folder per architecture. A CLAP
; is one file, so there are two, sharing the resources folder; each host skips the one it cannot load.
Source: "{#Arm64EcArtefactsDir}\VST3\{#ProductName}.vst3\Contents\arm64ec-win\*"; DestDir: "{code:GetVst3Dir}\Contents\arm64ec-win"; Excludes: *.ilk; Flags: ignoreversion recursesubdirs; Components: vst3
Source: "{#Arm64EcArtefactsDir}\CLAP\{#ProductName}.clap"; DestDir: "{code:GetClapDir}"; DestName: "{#ProductName} (ARM64EC).clap"; Flags: ignoreversion; Components: clap
; Its native UI's fonts, in the folder named after it (the arm64ec-win VST3 folder above carries its own).
Source: "{#Arm64EcArtefactsDir}\CLAP\resources\*.ttf"; DestDir: "{code:GetClapDir}\{#ProductName} (ARM64EC) Resources"; Flags: ignoreversion skipifsourcedoesntexist; Components: clap
#endif


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
    // Set when the user answers No to installing the WebView2 runtime (asked on the Ready page).
    WebView2Declined: Boolean;

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

#if IncludeAAX == "1"
function GetAaxDir(Param: string): string;
begin
    if Assigned(InstallPathsPage) and (Trim(InstallPathsPage.Values[3]) <> '') then
        Result := TrimmedPath(InstallPathsPage.Values[3])
    else
        Result := ReadSavedInstallPath('AAXDir', ExpandConstant('{#CommonFiles}\Avid\Audio\Plug-Ins\{#ProductName}.aaxplugin'));
end;
#endif

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
#if IncludeAAX == "1"

    InstallPathsPage.Add('AAX bundle folder (.aaxplugin):');
    InstallPathsPage.Values[3] := GetAaxDir('');
#endif
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
    Result := False;

    if (PageID = InstallPathsPage.ID) and (WizardSetupType(False) <> 'custom') then
        Result := True;
end;

function HasWebView2Version(const RootKey: Integer): Boolean;
var
    Version: string;
begin
    // An uninstalled runtime can leave its key behind with pv = 0.0.0.0.
    Result := RegQueryStringValue(RootKey, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version)
        and (Version <> '') and (Version <> '0.0.0.0');
end;

function IsWebView2RuntimeInstalled: Boolean;
begin
    // A per-machine runtime registers in the 32-bit view (WOW6432Node) on 64-bit Windows. In 64-bit
    // install mode HKLM is the 64-bit view, so HKLM32 has to be read explicitly: without it every
    // x64/ARM64 install missed the runtime and downloaded and re-ran its installer after copying files.
    Result := HasWebView2Version(HKLM32)
        or (IsWin64 and HasWebView2Version(HKLM64))
        or HasWebView2Version(HKCU);
end;

function NeedsWebView2Runtime: Boolean;
begin
    // Not wanted when only the native UI is used: it draws without a browser.
    Result := not IsWebView2RuntimeInstalled
        and not WizardIsTaskSelected('nativeuionly');
end;

// Asked before anything is installed, and only when the runtime is missing. A silent install takes
// the default (Yes); /TASKS="nativeuionly" leaves it out there.
procedure ConfirmWebView2Install;
begin
    WebView2Declined := False;

    if not WizardIsComponentSelected('standalone') or not NeedsWebView2Runtime then
        Exit;

    WebView2Declined := SuppressibleMsgBox(
        'The Microsoft Edge WebView2 runtime is not installed. {#ProductName} uses it to show its main interface.' + #13#10#13#10 +
        'Download and install it from Microsoft now? This needs an internet connection and can take a few minutes.' + #13#10#13#10 +
        'If you choose No, {#ProductName} uses its native interface until WebView2 is installed.',
        mbConfirmation, MB_YESNO, IDYES) <> IDYES;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
    Result := True;

    if CurPageID = wpReady then
    begin
        ConfirmWebView2Install;
        Exit;
    end;

    if CurPageID = wpSelectComponents then
    begin
        if not WizardIsComponentSelected('standalone')
            and not WizardIsComponentSelected('vst3')
            and not WizardIsComponentSelected('clap')
#if IncludeAAX == "1"
            and not WizardIsComponentSelected('aax')
#endif
            then
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
#if IncludeAAX == "1"

    if WizardIsComponentSelected('aax') and (Trim(GetAaxDir('')) = '') then
    begin
        MsgBox('AAX install folder cannot be empty.', mbError, MB_OK);
        Result := False;
        Exit;
    end;
#endif
end;

function InstallWebView2Runtime: Boolean;
var
    InstallerPath: string;
    DownloadCommand: string;
    ResultCode: Integer;
begin
    Result := True;

    if WebView2Declined or not NeedsWebView2Runtime then
        Exit;

    // The download and install below can take a minute or more with no file progress to show.
    WizardForm.StatusLabel.Caption := 'Installing the Microsoft Edge WebView2 runtime...';
    WizardForm.ProgressGauge.Style := npbstMarquee;

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
            Log('Continuing install without WebView2 runtime; the app and plugins will use the native UI until it is installed.');
    end;

    SharedUiPath := ExpandConstant('{commonappdata}\{#ProductName}\resources\ui');

end;
