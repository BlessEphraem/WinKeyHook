; -----------------------------------------------------------------------------
; WinKeyHook Daemon — Inno Setup Script
; -----------------------------------------------------------------------------

[Setup]
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
AppId={{B3F7A2D1-9C4E-4F8B-A6D5-2E1C0B5F8A3D}
AppName=WinKeyHook Daemon
AppVersion=1.1.0
AppPublisher=Ephraem
AppSupportURL=https://github.com/BlessEphraem/WinKeyHook
DefaultDirName={pf64}\Ephraem\Daemons
DisableProgramGroupPage=yes
DirExistsWarning=no
OutputDir=.
OutputBaseFilename=WinKeyHook_setup
Compression=lzma2/ultra
SolidCompression=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
DisableDirPage=yes
DisableReadyMemo=yes
DisableReadyPage=yes
DisableFinishedPage=yes
DisableWelcomePage=yes
UninstallDisplayIcon={app}\WinKeyHook.exe

[Files]
Source: "..\src\WinKeyHook.exe"; DestDir: "{app}"; Flags: ignoreversion


[Code]
var
  Page1: TOutputMsgWizardPage;
  Page2: TOutputMsgWizardPage;

{ ---- Helpers ---------------------------------------------------------------- }

function IsUpgrade(): Boolean;
begin
  Result := RegKeyExists(HKLM,
    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\' +
    '{B3F7A2D1-9C4E-4F8B-A6D5-2E1C0B5F8A3D}_is1');
end;

function IsInPath(const Path: string): Boolean;
var
  CurrentPath: string;
begin
  if not RegQueryStringValue(HKLM,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', CurrentPath) then
    CurrentPath := '';
  Result := Pos(LowerCase(Path), LowerCase(CurrentPath)) > 0;
end;

procedure AddToPath(const PathToAdd: string);
var
  CurrentPath: string;
begin
  if IsInPath(PathToAdd) then Exit;
  if not RegQueryStringValue(HKLM,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', CurrentPath) then
    CurrentPath := '';
  if CurrentPath <> '' then
    CurrentPath := CurrentPath + ';';
  RegWriteExpandStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', CurrentPath + PathToAdd);
end;

procedure RemoveFromPath(const PathToRemove: string);
var
  CurrentPath, NewPath, Lower, LowerRemove: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKLM,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', CurrentPath) then Exit;
  Lower        := LowerCase(CurrentPath);
  LowerRemove  := LowerCase(PathToRemove);

  P := Pos(';' + LowerRemove, Lower);
  if P > 0 then begin
    NewPath := Copy(CurrentPath, 1, P - 1) +
               Copy(CurrentPath, P + 1 + Length(PathToRemove), MaxInt);
    RegWriteExpandStringValue(HKLM,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', NewPath);
    Exit;
  end;

  P := Pos(LowerRemove + ';', Lower);
  if P = 1 then begin
    NewPath := Copy(CurrentPath, Length(PathToRemove) + 2, MaxInt);
    RegWriteExpandStringValue(HKLM,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', NewPath);
    Exit;
  end;

  if Lower = LowerRemove then
    RegWriteExpandStringValue(HKLM,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', '');
end;

{ ---- Install ---------------------------------------------------------------- }

procedure InitializeWizard;
begin
  if IsUpgrade() then Exit;

  Page1 := CreateOutputMsgPage(wpWelcome,
    'What You Are About To Install',
    'Please read this information carefully before continuing.',
    'You are about to install WinKeyHook.exe — a small background keyboard daemon.' + #13#10 +
    '' + #13#10 +
    'It runs silently in the background, uses approximately 10 MB of RAM, and starts' + #13#10 +
    'automatically with Windows. It has no visible interface and collects no data.' + #13#10 +
    '' + #13#10 +
    'If you are seeing this message without having downloaded anything related to' + #13#10 +
    '"WinKeyHook" yourself, a program you are currently installing requires it as a' + #13#10 +
    'dependency. This is expected — WinKeyHook is a shared background component.');

  Page2 := CreateOutputMsgPage(Page1.ID,
    'Administrator Privileges — Recommended Setup',
    'How to get the best experience with WinKeyHook.',
    'WinKeyHook requires Administrator privileges to reliably intercept keyboard' + #13#10 +
    'shortcuts for all applications, including those that run as Administrator' + #13#10 +
    '(such as games, creative tools, or professional software).' + #13#10 +
    '' + #13#10 +
    'To avoid a UAC prompt every time an application starts the daemon, this installer' + #13#10 +
    'automatically creates a Windows Task Scheduler entry that launches WinKeyHook at' + #13#10 +
    'logon with the required elevated privileges — no prompts, no manual steps.' + #13#10 +
    '' + #13#10 +
    'If you ever uninstall WinKeyHook, the uninstaller will cleanly remove all files,' + #13#10 +
    'registry entries, PATH entries, and the scheduled task. Nothing will be left behind.');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM WinKeyHook.exe /T',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(500);
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then begin
    AddToPath(ExpandConstant('{app}'));
    Exec(ExpandConstant('{sys}\schtasks.exe'),
         '/Create /F /TN "WinKeyHook" /RL HIGHEST /SC ONLOGON /TR ' +
         '"\"' + ExpandConstant('{app}') + '\WinKeyHook.exe\" 0"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

{ ---- Uninstall -------------------------------------------------------------- }

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM WinKeyHook.exe /T',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(500);
    Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN "WinKeyHook" /F',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    RemoveFromPath(ExpandConstant('{app}'));
  end;
end;
