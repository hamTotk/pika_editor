<#
.SYNOPSIS
  pika ポータブル版 zip を生成する（要件13・design doc sprint7）。

.DESCRIPTION
  Tauri bundler には zip ターゲットが無いため、ポータブル配布は本スクリプトで自前生成する。
  生成物は自己完結（pika.exe に frontend と同梱ベンダーアセット〔Mermaid/KaTeX/highlight〕が
  include_dir で埋め込み済み）。同梱物に portable.txt マーカーを置くことで、データルートが
  実行ホストの %LOCALAPPDATA% ではなく zip 展開先の ./pika-data/ になり、痕跡を残さず持ち運べる
  （pika-core::data_root）。ポータブル版はエクスプローラー統合を登録しない（要件13）。

  既定で frontend(npm run build) と Release ビルド(cargo build --release) を実行してから固める。
  既ビルド済みなら -SkipBuild で省略できる。

.PARAMETER SkipBuild
  npm/cargo のビルドを省略し、既存の target/release バイナリと dist をそのまま固める。

.PARAMETER OutDir
  zip の出力先（既定: installer/dist）。

.EXAMPLE
  pwsh installer/build-portable.ps1
  pwsh installer/build-portable.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
  [switch]$SkipBuild,
  [string]$OutDir
)

$ErrorActionPreference = 'Stop'

# リポジトリルート（このスクリプトは installer/ 直下）。
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ConfPath = Join-Path $RepoRoot 'src-tauri/tauri.conf.json'
if (-not (Test-Path $ConfPath)) { throw "tauri.conf.json が見つかりません: $ConfPath" }

# バージョンは tauri.conf.json を正とする。
$Version = (Get-Content $ConfPath -Raw | ConvertFrom-Json).version
if ([string]::IsNullOrWhiteSpace($Version)) { throw 'tauri.conf.json から version を取得できません' }
Write-Host "pika ポータブル版を生成します（version $Version）" -ForegroundColor Cyan

if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'installer/dist' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Push-Location $RepoRoot
try {
  if (-not $SkipBuild) {
    # frontend を先にビルドする（pika.exe が dist を埋め込むため Release ビルドより前に必須）。
    Write-Host '==> npm run build（frontend）' -ForegroundColor Yellow
    npm run build
    if ($LASTEXITCODE -ne 0) { throw 'npm run build に失敗しました' }

    # ⚠ pika.exe は本番モード（埋め込み dist を custom protocol で配信）にするため custom-protocol
    # feature が必須。付けないと Release でも devUrl(localhost:5173) を見にいき、Vite 未起動だと真っ白
    # （ERR_CONNECTION_REFUSED）になる＝`cargo tauri build` 相当を tauri-cli 無しで再現する。
    Write-Host '==> cargo build --release --features custom-protocol（pika）' -ForegroundColor Yellow
    cargo build --release -p pika-app --bin pika --features custom-protocol
    if ($LASTEXITCODE -ne 0) { throw 'pika.exe の Release ビルドに失敗しました' }
    # pika-cli は WebView 非依存（custom-protocol 不要）。
    Write-Host '==> cargo build --release（pika-cli）' -ForegroundColor Yellow
    cargo build --release -p pika-cli
    if ($LASTEXITCODE -ne 0) { throw 'pika-cli.exe の Release ビルドに失敗しました' }
  }

  $GuiExe = Join-Path $RepoRoot 'target/release/pika.exe'
  $CliExe = Join-Path $RepoRoot 'target/release/pika-cli.exe'
  $Notices = Join-Path $RepoRoot 'assets/THIRD_PARTY_NOTICES'
  $License = Join-Path $RepoRoot 'LICENSE'
  $InstallDoc = Join-Path $RepoRoot 'docs/install.md'
  foreach ($p in @($GuiExe, $CliExe, $Notices, $License)) {
    if (-not (Test-Path $p)) { throw "同梱物が見つかりません: $p（-SkipBuild なら先に通常ビルドが必要）" }
  }

  # ステージング（zip 内のトップフォルダ＝ pika-<version>-portable）。
  $StageRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("pika-portable-" + [System.Guid]::NewGuid().ToString('N'))
  $StageDir = Join-Path $StageRoot "pika-$Version-portable"
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

  Copy-Item $GuiExe (Join-Path $StageDir 'pika.exe')
  Copy-Item $CliExe (Join-Path $StageDir 'pika-cli.exe')
  Copy-Item $Notices (Join-Path $StageDir 'THIRD_PARTY_NOTICES.txt')
  Copy-Item $License (Join-Path $StageDir 'LICENSE.txt')
  if (Test-Path $InstallDoc) { Copy-Item $InstallDoc (Join-Path $StageDir 'はじめにお読みください.md') }

  # portable.txt マーカー（存在するだけでデータルートが ./pika-data/ になる）。
  $marker = @"
このファイル（portable.txt）が pika.exe と同じフォルダにある間、pika はポータブル版として動作します。
設定・状態・スナップショット・ログはこのフォルダ直下の pika-data\ に保存され、
実行した PC の %LOCALAPPDATA% には何も書き込みません（持ち運び可能・痕跡を残しません）。
ポータブル版はエクスプローラー統合（関連付け・右クリック）を登録しません。
"@
  Set-Content -Path (Join-Path $StageDir 'portable.txt') -Value $marker -Encoding UTF8

  $ZipPath = Join-Path $OutDir "pika-$Version-portable.zip"
  if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
  Write-Host "==> zip 生成: $ZipPath" -ForegroundColor Yellow
  # $StageDir 自体（末尾 \* を付けない）を固める＝zip ルートに pika-<version>-portable\ トップフォルダが
  # 入る。`\*` だと中身だけが固められ、展開時に任意フォルダを直接汚染する（トップフォルダが無い）。
  Compress-Archive -Path $StageDir -DestinationPath $ZipPath -CompressionLevel Optimal

  Remove-Item $StageRoot -Recurse -Force
  Write-Host "完了: $ZipPath" -ForegroundColor Green
}
finally {
  Pop-Location
}
