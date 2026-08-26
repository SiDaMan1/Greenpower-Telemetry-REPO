; ════════════════════════════════════════════════════════════════════
;  GREENPOWER RECEIVER AGENT — Inno Setup installer script
;
;  Replaces the old "unzip + double-click setup.bat" flow with a real
;  Windows installer: Start Menu shortcut, entry in Add/Remove Programs,
;  proper uninstaller. It still does the SAME underlying work setup.bat
;  did (npm install, register hidden auto-start at login, launch now) —
;  this is a packaging change, not a behavior change to the agent itself.
;
;  PrivilegesRequired=lowest + a {localappdata} install dir means this
;  needs NO ADMIN RIGHTS AND NO UAC PROMPT — deliberate, since the
;  person running this on a laptop in the pits may not have admin on
;  that machine. Don't change DefaultDirName to a Program Files path or
;  add AppMutex-style elevation without a real reason; that would
;  reintroduce the exact UAC friction this was built to avoid.
;
;  Node.js itself is still a REQUIRED PREREQUISITE — npm install runs
;  at install time (needs Node/npm on PATH + internet), same as
;  setup.bat always required. This installer does not bundle a Node.js
;  runtime or try to build a fully standalone .exe of the agent — that
;  was considered and deliberately not attempted: this agent depends on
;  serialport (a native addon) and systray (spawns its own helper
;  binary), and bundling native addons into a single-file executable
;  (via pkg/Node's SEA feature) is a known-fragile combination that
;  could not be reliably verified working in this environment without a
;  real target-machine test. If Node.js is ever bundled/vendored
;  instead of assumed-present, that's a deliberate follow-up, not
;  something to add casually — re-verify serialport/systray still work
;  from whatever new packaging is used.
;
;  BUILD: from this folder, run (no admin needed):
;    "%LocalAppData%\Programs\Inno Setup 7\ISCC.exe" GreenpowerAgent.iss
;  Output lands in installer\dist\GreenpowerAgentSetup.exe — copy that
;  into telemetry_web\public\ to publish it as the website's download
;  (see telemetry_web\CLAUDE.md for the exact copy step, mirroring how
;  greenpower-agent.zip used to be rebuilt by hand).
; ════════════════════════════════════════════════════════════════════

#define MyAppName "Greenpower Receiver Agent"
#define MyAppVersion "1.2"
#define MyAppPublisher "Greenpower"
#define MyAppExeName "agent.js"

[Setup]
AppId={{9F1B1A2E-6C6C-4E8B-9C4C-2E9D6B7A2F31}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; Per-user, no-admin install — see the header comment above for why this
; is load-bearing, not just a default left unchanged.
DefaultDirName={localappdata}\Programs\GreenpowerAgent
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
DisableWelcomePage=no
OutputDir=dist
OutputBaseFilename=GreenpowerAgentSetup
Compression=lzma2
SolidCompression=yes
SetupIconFile=..\tray-icon.ico
UninstallDisplayIcon={app}\tray-icon.ico
WizardStyle=modern
InfoBeforeFile=infobefore.txt
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\agent.js"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\package.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\package-lock.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\tray-icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\CLAUDE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\config.example.json"; DestDir: "{app}"; Flags: ignoreversion
; onlyifdoesntexist — a re-install/upgrade must never clobber a config a
; user has already customized, mirroring setup.bat's own "leave it as-is
; if config.json already exists" behavior.
Source: "..\config.json"; DestDir: "{app}"; Flags: ignoreversion onlyifdoesntexist

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\tray-icon.ico"; IconFilename: "{app}\tray-icon.ico"

[Code]
var
  StartupLauncherPath: String;

// Writes the same kind of hidden VBS launcher setup.bat used to hand-write
// (WshShell.Run "node agent.js", 0, False — the ", 0" is what makes it run
// with no visible console window). Generated here instead of shipped as a
// static [Files] entry because it needs {app}'s actual install path baked
// into it, which is only known at install time.
procedure WriteStartupLauncher();
var
  Lines: TArrayOfString;
begin
  StartupLauncherPath := ExpandConstant('{userstartup}\GreenpowerReceiverAgent.vbs');
  SetArrayLength(Lines, 3);
  Lines[0] := 'Set WshShell = CreateObject("WScript.Shell")';
  Lines[1] := 'WshShell.CurrentDirectory = "' + ExpandConstant('{app}') + '"';
  Lines[2] := 'WshShell.Run "node agent.js", 0, False';
  SaveStringsToFile(StartupLauncherPath, Lines, False);
end;

// Same idempotency guard setup.bat had via agent.pid: if a previous
// install's agent is still running, stop it before starting the new one,
// so a re-install/upgrade doesn't end up with two node.exe processes
// fighting over the same serial port.
procedure StopExistingAgent();
var
  PidFile: String;
  PidStr: AnsiString;
  ResultCode: Integer;
begin
  PidFile := ExpandConstant('{app}\agent.pid');
  if FileExists(PidFile) then begin
    if LoadStringFromFile(PidFile, PidStr) then begin
      PidStr := Trim(PidStr);
      if PidStr <> '' then
        Exec('taskkill.exe', '/F /PID ' + PidStr, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;
    DeleteFile(PidFile);
  end;
end;

function NodeIsOnPath(): Boolean;
var
  ResultCode: Integer;
begin
  // "where node" exits 0 if found, nonzero if not — cheaper and more
  // reliable than trying to parse PATH ourselves.
  Result := Exec('cmd.exe', '/c where node >nul 2>nul', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
    and (ResultCode = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then begin
    if not NodeIsOnPath() then begin
      MsgBox('Node.js was not found on this computer.' + #13#10 + #13#10 +
        'This agent needs Node.js (LTS) to run. Install it from https://nodejs.org, ' +
        'then run this setup again — everything else is already in place.',
        mbInformation, MB_OK);
      Exit;
    end;

    StopExistingAgent();

    WizardForm.StatusLabel.Caption := 'Installing dependencies (requires internet)...';
    if not Exec('cmd.exe', '/c npm install', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode)
       or (ResultCode <> 0) then begin
      MsgBox('npm install failed (exit code ' + IntToStr(ResultCode) + '). ' +
        'Check your internet connection, then run this setup again.',
        mbError, MB_OK);
      Exit;
    end;

    WriteStartupLauncher();
    Exec('wscript.exe', '"' + StartupLauncherPath + '"', '', SW_HIDE, ewNoWait, ResultCode);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  PidFile, LauncherPath: String;
  PidStr: AnsiString;
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    PidFile := ExpandConstant('{app}\agent.pid');
    if FileExists(PidFile) then begin
      if LoadStringFromFile(PidFile, PidStr) then begin
        PidStr := Trim(PidStr);
        if PidStr <> '' then
          Exec('taskkill.exe', '/F /PID ' + PidStr, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      end;
    end;
    LauncherPath := ExpandConstant('{userstartup}\GreenpowerReceiverAgent.vbs');
    if FileExists(LauncherPath) then
      DeleteFile(LauncherPath);
  end;
end;

[Run]
Filename: "{app}\README.md"; Description: "View the README"; Flags: postinstall shellexec skipifsilent unchecked
