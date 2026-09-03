#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef AppVersionInfo
  #error AppVersionInfo is required
#endif
#ifndef InstallerAppId
  #error InstallerAppId is required
#endif
#ifndef PayloadRoot
  #error PayloadRoot is required
#endif
#ifndef ReleaseOutput
  #error ReleaseOutput is required
#endif
#ifndef SetupBaseName
  #error SetupBaseName is required
#endif
; Release builds use these defaults. The focused installer regression harness
; overrides them to keep its HKCU keys and mutexes isolated from a real install.
#ifndef ProductRegistryKey
  #define ProductRegistryKey "Software\underfusion\GameHQ"
#endif
#ifndef AppPathRegistryKey
  #define AppPathRegistryKey "Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ.exe"
#endif
#ifndef RunRegistryKey
  #define RunRegistryKey "Software\Microsoft\Windows\CurrentVersion\Run"
#endif
#ifndef RunRegistryValue
  #define RunRegistryValue "GameHQ"
#endif
#ifndef ApplicationMutexValue
  #define ApplicationMutexValue "Local\GameHQApplicationActive"
#endif
#ifndef UpdaterMutexValue
  #define UpdaterMutexValue "Local\GameHQUpdaterActive"
#endif
#ifndef BootstrapProfileRelativePath
  #define BootstrapProfileRelativePath "GameHQ\config.json"
#endif

[Setup]
AppId={{#InstallerAppId}
AppName=GameHQ
AppVersion={#AppVersion}
AppVerName=GameHQ {#AppVersion}
VersionInfoVersion={#AppVersionInfo}
VersionInfoCompany=underfusion
VersionInfoDescription=GameHQ for Windows Setup
VersionInfoProductName=GameHQ
AppPublisher=underfusion
AppPublisherURL=https://github.com/underfusion/GameHQ
AppSupportURL=https://github.com/underfusion/GameHQ/issues
AppUpdatesURL=https://github.com/underfusion/GameHQ/releases
DefaultDirName={localappdata}\Programs\GameHQ
DefaultGroupName=GameHQ
PrivilegesRequired=lowest
Uninstallable=yes
UsePreviousAppDir=yes
CloseApplications=no
RestartApplications=no
AppMutex={code:ApplicationMutexName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
OutputDir={#ReleaseOutput}
OutputBaseFilename={#SetupBaseName}
SetupIconFile={#SourcePath}\..\assets\icons\gamehq.ico
WizardImageFile={#SourcePath}\..\assets\installer\wizard-large.png
WizardSmallImageFile={#SourcePath}\..\assets\installer\wizard-small.png
UninstallDisplayIcon={app}\GameHQ.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
DisableWelcomePage=no
AllowNoIcons=yes

#include "generated\InnoLanguages.iss"

[Messages]
WelcomeLabel1={cm:GameHQWelcomeTitle}
WelcomeLabel2={cm:GameHQWelcomeBody}

#include "generated\InnoCustomMessages.iss"

[Files]
Source: "{#PayloadRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\GameHQ"; Filename: "{app}\GameHQ.exe"
Name: "{autodesktop}\GameHQ"; Filename: "{app}\GameHQ.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:GameHQDesktopShortcut}"; GroupDescription: "{cm:GameHQAdditionalShortcuts}"; Flags: unchecked

[Registry]
Root: HKCU; Subkey: "{#ProductRegistryKey}"; ValueType: string; ValueName: "InstallLocation"; ValueData: "{app}"
Root: HKCU; Subkey: "{#ProductRegistryKey}"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"
Root: HKCU; Subkey: "{#AppPathRegistryKey}"; ValueType: string; ValueName: ""; ValueData: "{app}\GameHQ.exe"
Root: HKCU; Subkey: "{#AppPathRegistryKey}"; ValueType: string; ValueName: "Path"; ValueData: "{app}"

[Run]
Filename: "{app}\GameHQ.exe"; Description: "{cm:GameHQLaunch}"; Flags: nowait postinstall skipifsilent

[Code]
const
  ExitAppRunning = 20;
  ExitUpdateActive = 21;
  { Mirrors maintenance::State in src/core/UpdateMaintenance.h. }
  MaintenanceInactive = 0;
  MaintenanceActive = 1;
  MaintenanceStale = 2;
  { Same window as the staleAfter default in maintenance::inspect. }
  MaintenanceStaleAfterSecs = 300;

var
  BootstrapWasOffered: Boolean;
  BootstrapHadValue: Boolean;
  BootstrapExistingInstall: Boolean;
  BootstrapExistingProfile: Boolean;

procedure GetSystemTimeAsFileTime(var FileTime: TFileTime);
  external 'GetSystemTimeAsFileTime@kernel32.dll stdcall';

procedure ExitProcess(ExitCode: Integer);
  external 'ExitProcess@kernel32.dll stdcall';

#include "generated\InnoLanguageBootstrap.iss"

function InitializeSetup: Boolean;
begin
  Result := True;
  BootstrapWasOffered := RegValueExists(HKCU, '{#ProductRegistryKey}',
    'BootstrapLanguageOffered');
  BootstrapHadValue := RegValueExists(HKCU, '{#ProductRegistryKey}',
    'BootstrapLanguage');
  BootstrapExistingInstall :=
    RegValueExists(HKCU, '{#ProductRegistryKey}', 'InstallLocation') or
    RegValueExists(HKCU, '{#ProductRegistryKey}', 'Version');
  BootstrapExistingProfile := FileExists(
    ExpandConstant('{userappdata}\{#BootstrapProfileRelativePath}'));
end;

procedure FinalizeLanguageBootstrap;
var
  AppLocale: String;
begin
  { A portable profile owns its locale entirely inside the package. }
  if FileExists(ExpandConstant('{app}\portable.flag')) then
    Exit;

  { A pending first-launch value survives upgrades. Once the app consumes it,
    the marker remains and Setup must never recreate the value. }
  if BootstrapWasOffered then
    Exit;

  { Pre-existing installs/profiles predate the marker, while a bare value is
    stale or foreign state. Suppress the offer and discard only that stale
    value; never inspect or edit config.json. }
  if BootstrapExistingInstall or BootstrapExistingProfile or BootstrapHadValue then
  begin
    if BootstrapHadValue and
       not RegDeleteValue(HKCU, '{#ProductRegistryKey}', 'BootstrapLanguage') then
      Log('Could not discard stale BootstrapLanguage; the app will validate it.');
    if not RegWriteDWordValue(HKCU, '{#ProductRegistryKey}',
       'BootstrapLanguageOffered', 1) then
      Log('Could not record that the installer language offer was suppressed.');
    Exit;
  end;

  AppLocale := CanonicalLocaleForInstallerLanguage(ActiveLanguage);
  if (AppLocale <> '') and
     not RegWriteStringValue(HKCU, '{#ProductRegistryKey}',
       'BootstrapLanguage', AppLocale) then
    Log('Could not write BootstrapLanguage; the app will use its normal fallback.');
  if not RegWriteDWordValue(HKCU, '{#ProductRegistryKey}',
     'BootstrapLanguageOffered', 1) then
    Log('Could not record that the installer language was offered.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    FinalizeLanguageBootstrap;
end;

procedure FailSilent(ExitCode: Integer);
begin
  if WizardSilent then
    ExitProcess(ExitCode);
end;

function ApplicationMutexName(Param: String): String;
begin
  { Interactive Setup and Uninstall use Inno's native AppMutex prompt. Silent
    Setup reaches PrepareToInstall so automation receives reserved code 20. }
  if WizardSilent then
    Result := ''
  else
    Result := '{#ApplicationMutexValue}';
end;

{ The app and updater mutexes are per-session (Local\), so a copy of GameHQ
  running in another Windows session - Fast User Switching, or a second session
  of the same account - is invisible to them. The installed executable itself is
  not: while it runs, its image file cannot be opened exclusively. }
function FileIsInUse(const Path: String): Boolean;
var
  Stream: TFileStream;
begin
  Result := False;
  if not FileExists(Path) then
    Exit;
  try
    Stream := TFileStream.Create(Path, fmOpenRead or fmShareExclusive);
    Stream.Free;
  except
    Result := True;
  end;
end;

function ApplicationIsRunning(const AppDir: String): Boolean;
begin
  Result := CheckForMutexes('{#ApplicationMutexValue}')
            or FileIsInUse(AppDir + '\app\GameHQ.exe')
            or FileIsInUse(AppDir + '\GameHQUpdater.exe');
end;

function ReadTransactionPhase(const AppDir: String): String;
var
  Lines: TArrayOfString;
begin
  Result := '';
  if LoadStringsFromFile(AppDir + '\.update\transaction.phase', Lines) then
    if GetArrayLength(Lines) > 0 then
      Result := Trim(Lines[0]);
end;

function FileTimeToInt64(const Value: TFileTime): Int64;
begin
  Result := Int64(Value.dwHighDateTime) * 4294967296 + Int64(Value.dwLowDateTime);
end;

{ Mirrors maintenance::inspect in src/core/UpdateMaintenance.cpp. The marker
  alone means nothing: the updater writes a terminal phase before clearing it,
  and a marker left by a crash must not block Setup forever - which is exactly
  what testing the file's existence used to do. }
function MaintenanceState(const AppDir: String; var Phase: String): Integer;
var
  Marker: String;
  FindRec: TFindRec;
  NowTime: TFileTime;
begin
  Phase := '';
  Result := MaintenanceInactive;
  Marker := AppDir + '\.update\maintenance.lock';
  if not FileExists(Marker) then
    Exit;

  Phase := ReadTransactionPhase(AppDir);
  if (Phase = 'healthy') or (Phase = 'rolled_back') then
    Exit;   { finished work waiting to be cleaned up }

  Result := MaintenanceActive;
  if CheckForMutexes('{#UpdaterMutexValue}') then
    Exit;

  if FindFirst(Marker, FindRec) then
  try
    GetSystemTimeAsFileTime(NowTime);
    if (FileTimeToInt64(NowTime) - FileTimeToInt64(FindRec.LastWriteTime))
         > Int64(MaintenanceStaleAfterSecs) * 10000000 then
      Result := MaintenanceStale;
  finally
    FindClose(FindRec);
  end;
end;

{ Empty when nothing blocks. Never deletes the marker or the phase file: they
  are the evidence GameHQ's own recovery needs. }
function MaintenanceBlockReason(const AppDir: String): String;
var
  Phase: String;
  State: Integer;
  Detail: String;
begin
  Result := '';
  State := MaintenanceState(AppDir, Phase);
  if State = MaintenanceInactive then
    Exit;

  Detail := '';
  if Phase <> '' then
    Detail := FmtMessage(CustomMessage('GameHQStageDetail'), [Phase]);

  if State = MaintenanceActive then
    Result := FmtMessage(CustomMessage('GameHQUpdateActive'), [Detail])
  else
    Result := FmtMessage(CustomMessage('GameHQUpdateStale'), [Detail])
              + #13#10 + CustomMessage('GameHQNothingRemoved');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  AppDir: String;
begin
  Result := '';
  AppDir := ExpandConstant('{app}');
  if ApplicationIsRunning(AppDir) then
  begin
    FailSilent(ExitAppRunning);
    Result := CustomMessage('GameHQSetupAppRunning');
    Exit;
  end;
  Result := MaintenanceBlockReason(AppDir);
  if Result <> '' then
    FailSilent(ExitUpdateActive);
end;

{ Uninstall used to check only the application mutex, so it would happily
  delete an installation out from under a running update. Inno's outer
  uninstaller exposes only zero versus nonzero even when this inner clone exits
  with a reserved reason code. }
function InitializeUninstall: Boolean;
var
  AppDir, Reason: String;
begin
  Result := True;
  AppDir := ExpandConstant('{app}');
  if ApplicationIsRunning(AppDir) then
  begin
    if UninstallSilent then
      ExitProcess(ExitAppRunning);
    MsgBox(CustomMessage('GameHQUninstallAppRunning'), mbError, MB_OK);
    Result := False;
    Exit;
  end;
  Reason := MaintenanceBlockReason(AppDir);
  if Reason <> '' then
  begin
    if UninstallSilent then
      ExitProcess(ExitUpdateActive);
    MsgBox(Reason, mbError, MB_OK);
    Result := False;
  end;
end;

function CommandTargetsThisInstall(CommandLine: String): Boolean;
var
  Candidate: String;
  ClosingQuote, SpaceAt: Integer;
begin
  Result := False;
  Candidate := Trim(CommandLine);
  if Candidate = '' then
    Exit;

  if Candidate[1] = '"' then
  begin
    Delete(Candidate, 1, 1);
    ClosingQuote := Pos('"', Candidate);
    if ClosingQuote = 0 then
      Exit;
    Candidate := Copy(Candidate, 1, ClosingQuote - 1);
  end
  else
  begin
    SpaceAt := Pos(' ', Candidate);
    if SpaceAt > 0 then
      Candidate := Copy(Candidate, 1, SpaceAt - 1);
  end;

  Result := CompareText(ExpandFileName(Candidate),
    ExpandFileName(ExpandConstant('{app}\GameHQ.exe'))) = 0;
end;

procedure RemoveOwnedIntegration;
var
  Value: String;
  ProductKey, AppPathKey, RunKey: String;
begin
  ProductKey := '{#ProductRegistryKey}';
  AppPathKey := '{#AppPathRegistryKey}';
  RunKey := '{#RunRegistryKey}';

  if RegQueryStringValue(HKCU, ProductKey, 'InstallLocation', Value) and
     (CompareText(RemoveBackslashUnlessRoot(Value),
       RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0) then
  begin
    RegDeleteValue(HKCU, ProductKey, 'InstallLocation');
    RegDeleteValue(HKCU, ProductKey, 'Version');
    RegDeleteKeyIfEmpty(HKCU, ProductKey);
  end;

  if RegQueryStringValue(HKCU, AppPathKey, '', Value) and
     (CompareText(ExpandFileName(Value),
       ExpandFileName(ExpandConstant('{app}\GameHQ.exe'))) = 0) then
  begin
    RegDeleteValue(HKCU, AppPathKey, '');
    RegDeleteValue(HKCU, AppPathKey, 'Path');
    RegDeleteKeyIfEmpty(HKCU, AppPathKey);
  end;

  if RegQueryStringValue(HKCU, RunKey, '{#RunRegistryValue}', Value) and
     CommandTargetsThisInstall(Value) then
    RegDeleteValue(HKCU, RunKey, '{#RunRegistryValue}');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveOwnedIntegration;
end;
