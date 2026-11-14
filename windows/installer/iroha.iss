; iroha for Windows のインストーラー（Inno Setup 6）。
;   windows\scripts\make-installer.ps1 が一式を集めてから、次の形でコンパイルする:
;   ISCC.exe /DAppVersion=0.18.0 /DStageDir=<集めたフォルダ> /DOutputDir=<出力先> windows\installer\iroha.iss
; 出力は iroha-<版>-windows-x64-setup.exe。GitHub の Release に置き、設定画面のアップデート（updater.cpp）が
; 同じものを /SILENT で起動して入れ替える。
;
; 入れるもの: Program Files\iroha に TIP（iroha-tip.dll、TSF に登録）・変換サーバ・設定画面・辞書・記号のデータ・
; ライセンス。変換サーバはログオン時に起動する（HKLM の Run）。変換モデル iroha-kkc-v1 が無ければ取得して
; %LOCALAPPDATA%\iroha\models に置く（大きさと SHA-256 は macOS版の ModelDownloader.swift と同じ）。
; 学習・ユーザ辞書・設定（%LOCALAPPDATA%\iroha）はアンインストールしても消さない。

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef StageDir
  #error StageDir を指定してください（/DStageDir=...）
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
; ファイルの版の情報は数だけ（0.18.0-beta.1 なら 0.18.0）
#define VersionNumbers Copy(AppVersion, 1, Pos("-", AppVersion + "-") - 1)

#define ModelFile "iroha-kkc-v1-Q5_K_M.gguf"
#define ModelUrl "https://github.com/TetsuakiBaba/iroha/releases/download/iroha-kkc-v1/iroha-kkc-v1-Q5_K_M.gguf"
#define ModelSha256 "f060e259dbed28754f71e1634bc01a5d9d735d519f8ff72c333cac77c859efda"

[Setup]
; AppId は変えない（変えると別のアプリとして入り、アップデートで上書きされなくなる）
AppId={{7C3E2A55-9B1D-4E8F-A6C2-5D0F1B4E9A73}
AppName=iroha
AppVersion={#AppVersion}
AppVerName=iroha {#AppVersion}
AppPublisher=Tetsuaki Baba
AppPublisherURL=https://github.com/TetsuakiBaba/iroha
AppSupportURL=https://github.com/TetsuakiBaba/iroha/issues
AppUpdatesURL=https://github.com/TetsuakiBaba/iroha/releases
VersionInfoVersion={#VersionNumbers}
VersionInfoProductVersion={#VersionNumbers}
VersionInfoDescription=iroha インストーラー
; TIP の登録先（HKLM）と Program Files に書くので管理者で動く。普段のユーザで起動しても自分で許可を求める
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DefaultDirName={autopf}\iroha
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
; TIP は入力するすべてのアプリに読み込まれているので、使っているアプリを閉じる仕組みは使わない
; （DLL は名前を変えて退避してから新しいものを置く。PrepareToInstall）
CloseApplications=no
RestartApplications=no
SetupIconFile={#StageDir}\iroha.ico
UninstallDisplayIcon={app}\iroha-settings.exe
UninstallDisplayName=iroha
WizardStyle=modern
ShowLanguageDialog=no
Compression=lzma2/max
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=iroha-{#AppVersion}-windows-x64-setup

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Messages]
japanese.FinishedLabel=iroha をインストールしました。%n%n設定 > 時刻と言語 > 言語と地域 > 日本語 > 言語のオプション > キーボード に「iroha」が現れます。現れないときはサインアウトしてサインインし直してください。Win+Space で切り替えられます。%n%n起動中のアプリは、開き直すまで前の版の iroha を使います。

[Files]
Source: "{#StageDir}\iroha-tip.dll"; DestDir: "{app}"; Flags: ignoreversion regserver uninsrestartdelete
Source: "{#StageDir}\iroha-server.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\iroha-settings.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\iroha-bench.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\Dictionary\*"; DestDir: "{app}\Dictionary"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\Symbols\*"; DestDir: "{app}\Symbols"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; 辞書・記号のデータは版ごとに丸ごと入れ替える（前の版にだけあったファイルを残さない）
Type: filesandordirs; Name: "{app}\Dictionary"
Type: filesandordirs; Name: "{app}\Symbols"
Type: filesandordirs; Name: "{app}\licenses"
; 開発用のインストール（install-tip.ps1）が置いたもの
Type: files; Name: "{app}\*.pdb"

[Registry]
; 変換サーバをログオン時に起動する（ストアアプリの中の TIP は変換サーバを起動できないので、常駐させておく）
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "iroha-server"; ValueData: """{app}\iroha-server.exe"""; Flags: uninsdeletevalue
; 以前の手作業のインストール（install.ps1・install-tip.ps1）が HKCU に書いた同じ登録を消す（二重に起動しない）
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "iroha-server"; Flags: deletevalue

[Icons]
Name: "{autoprograms}\iroha の設定"; Filename: "{app}\iroha-settings.exe"

[Run]
; 管理者で動いているので、変換サーバは普段のユーザの権限で起動する
Filename: "{app}\iroha-server.exe"; Flags: nowait runasoriginaluser
Filename: "{app}\iroha-settings.exe"; Description: "iroha の設定を開く"; Flags: postinstall nowait skipifsilent runasoriginaluser unchecked

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM iroha-server.exe"; Flags: runhidden; RunOnceId: "StopServer"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM iroha-settings.exe"; Flags: runhidden; RunOnceId: "StopSettings"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM iroha-bench.exe"; Flags: runhidden; RunOnceId: "StopBench"

[UninstallDelete]
Type: files; Name: "{app}\iroha-tip.dll.old-*"

[Code]
var
  DownloadPage: TDownloadWizardPage;

function ModelPath(): String;
begin
  Result := ExpandConstant('{localappdata}\iroha\models\{#ModelFile}');
end;

procedure StopProcess(const Name: String);
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ' + Name, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

// 読み込まれている DLL は上書きできないが名前は変えられるので、退避してから新しいものを置く。
// 退避したものは、読み込んでいるアプリがなくなったあとの次のインストールで消える
procedure SetAsideLoadedDll();
var
  Dll: String;
  Find: TFindRec;
begin
  Dll := ExpandConstant('{app}\iroha-tip.dll');
  if FindFirst(Dll + '.old-*', Find) then
  begin
    try
      repeat
        DeleteFile(ExpandConstant('{app}\') + Find.Name);
      until not FindNext(Find);
    finally
      FindClose(Find);
    end;
  end;
  if FileExists(Dll) then
    RenameFile(Dll, Dll + '.old-' + GetDateTimeString('yyyymmddhhnnss', #0, #0));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopProcess('iroha-server.exe');
  StopProcess('iroha-settings.exe');
  StopProcess('iroha-bench.exe');
  SetAsideLoadedDll();
  Result := '';
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), '変換モデル iroha-kkc-v1（約 82MB）をダウンロードしています', @OnDownloadProgress);
end;

// 変換モデルを取得して置く（照合に通ったものだけ）。取れなくてもインストールは続ける（設定 > モデル で取得できる）
function InstallModel(): Boolean;
begin
  Result := False;
  try
    if WizardSilent() then
      DownloadTemporaryFile('{#ModelUrl}', '{#ModelFile}', '{#ModelSha256}', nil)
    else
    begin
      DownloadPage.Clear;
      DownloadPage.Add('{#ModelUrl}', '{#ModelFile}', '{#ModelSha256}');
      DownloadPage.Show;
      try
        DownloadPage.Download;
      finally
        DownloadPage.Hide;
      end;
    end;
    ForceDirectories(ExtractFileDir(ModelPath()));
    Result := FileCopy(ExpandConstant('{tmp}\{#ModelFile}'), ModelPath(), False);
  except
    Log('model download failed: ' + GetExceptionMessage);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and not FileExists(ModelPath()) then
  begin
    if not InstallModel() and not WizardSilent() then
      MsgBox('変換モデルをダウンロードできませんでした。インストールのあと、iroha の設定 > モデル の「ダウンロード」で取得してください。', mbInformation, MB_OK);
  end;
end;
