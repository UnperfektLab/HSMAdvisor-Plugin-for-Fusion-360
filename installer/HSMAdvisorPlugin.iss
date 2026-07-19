; Inno Setup script for "HSMAdvisor Plugin for Fusion 360".
; Build with:  ISCC.exe HSMAdvisorPlugin.iss     (or run ..\build.ps1 -Package)
; Packages the files assembled by build.ps1 into ..\dist\HSMAdvisor Plugin.

#define MyName "HSMAdvisor Plugin for Fusion 360"
#define AddinName "HSMAdvisor Plugin"
#define MyVersion "0.2.0"
#define MyPublisher "UnperfektLab"
#define MyUrl "https://github.com/UnperfektLab/HSMAdvisor-Plugin-for-Fusion-360"
#define DistDir "..\dist\HSMAdvisor Plugin"

[Setup]
AppId={{A1B2C3D4-E5F6-47A8-B9C0-D1E2F3A4B5C6}
AppName={#MyName}
AppVersion={#MyVersion}
AppPublisher={#MyPublisher}
AppSupportURL={#MyUrl}
; Fusion add-ins live per-user under %APPDATA%; no admin needed.
PrivilegesRequired=lowest
DefaultDirName={userappdata}\Autodesk\Autodesk Fusion 360\API\AddIns\{#AddinName}
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallDisplayName={#MyName}
OutputDir=Output
OutputBaseFilename=HSMAdvisor-Plugin-for-Fusion360-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

[Files]
Source: "{#DistDir}\{#AddinName}.dll";            DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\HSMAdvisorPluginHost.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\{#AddinName}.manifest";       DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\AddInIcon.svg";               DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\resources\HSMAdvisorPlugin\*";  DestDir: "{app}\resources\HSMAdvisorPlugin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#DistDir}\README.md";                   DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#DistDir}\LICENSE";                     DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Remove runtime files the plugin creates (settings.xml, apply_prefs.txt) and the
; now-empty add-in folder on uninstall.
[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
const
  NL = #13#10;

function HSMAdvisorInstalled(): Boolean;
begin
  Result :=
    FileExists(ExpandConstant('{localappdata}\Programs\HSMAdvisor\HSMAdvisorCore.dll')) or
    FileExists(ExpandConstant('{commonpf}\HSMAdvisor\HSMAdvisorCore.dll')) or
    FileExists(ExpandConstant('{commonpf64}\HSMAdvisor\HSMAdvisorCore.dll'));
end;

function FusionPresent(): Boolean;
begin
  Result := DirExists(ExpandConstant('{userappdata}\Autodesk\Autodesk Fusion 360\API\AddIns'));
end;

function InitializeSetup(): Boolean;
begin
  Result := True;

  if not FusionPresent() then
    if MsgBox('Autodesk Fusion 360 was not detected for this user.' + NL +
              'The add-in will still be installed, but Fusion 360 must be installed to use it.' +
              NL + NL + 'Continue?', mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
      Exit;
    end;

  if not HSMAdvisorInstalled() then
    if MsgBox('HSMAdvisor does not appear to be installed.' + NL +
              'This add-in requires HSMAdvisor (https://hsmadvisor.com/download) to be' +
              NL + 'installed and licensed on this PC.' + NL + NL +
              'Install the add-in anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    MsgBox('Installed.' + NL + NL +
           'Restart Fusion 360. The "HSMAdvisor" button appears in the Manufacture' + NL +
           'workspace, in the Manage panel (Milling and Turning tabs).' + NL + NL +
           'If it does not load: Utilities > ADD-INS > Scripts and Add-Ins > HSMAdvisor Plugin > Run.',
           mbInformation, MB_OK);
end;
