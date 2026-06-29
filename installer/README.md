# installer/ — 配布物の生成

pika の配布物（要件13・design doc 9章/sprint7）をここで管理する。配布は2形態:

1. **ユーザー単位インストーラー（NSIS・管理者不要）** — Tauri bundler が生成
2. **ポータブル zip** — `build-portable.ps1` が自前生成（Tauri に zip ターゲットが無いため）

両形態とも pika.exe は自己完結（frontend と同梱ベンダーアセット〔Mermaid/KaTeX/highlight・約3.84MiB〕を
`include_dir` で exe へ埋め込み済み）。コード署名なし・自動更新なし（CLAUDE.md 確定判断）。

## 1. NSIS インストーラー

設定は `src-tauri/tauri.conf.json` の `bundle` セクション。要点:

- `bundle.windows.nsis.installMode = "both"` … インストール時にユーザー単位（管理者不要・既定推奨）／
  全ユーザー（要管理者）を選択。**注意**: 関連付け（HKCU）とデータ（`%LOCALAPPDATA%`）はユーザー領域のみ＝
  全ユーザー導入時もインストール実行ユーザーにのみ適用（全ユーザー HKLM 登録は未対応）
- `bundle.windows.nsis.installerHooks = "nsis/hooks.nsh"` … インストール/アンインストール時の処理
  - 完了後: `pika-cli.exe --register-shell`（エクスプローラー統合・要件3.3）
  - 解除前: `pika-cli.exe --unregister-shell`（関連付けの残骸を残さない）
  - 解除後: 設定・状態・ログを削除し、退避スナップショットは残す/消すを選ばせる（既定=残す・要件13）
- `bundle.resources` … `THIRD_PARTY_NOTICES.txt` を同梱（About 画面は exe 埋め込み版を表示）
- `bundle.windows.webviewInstallMode = downloadBootstrapper` … WebView2 不在時のみ取得（Win11 は同梱済みでほぼ非発火）

**サイドカー（pika-cli.exe）の同梱はオーバーレイで切り離す**: `externalBin` はサイドカー実体の存在を
ビルド時に検証するため、base の `tauri.conf.json` に置くと `cargo build` / `cargo test`（dev ゲート）が
壊れる。よってバンドル専用設定 `src-tauri/tauri.bundle.conf.json`（`active:true` ＋ `externalBin`）へ分離し、
**バンドル時だけ** `--config` で深いマージして有効化する（単一ソースは保持＝NSIS は pika-cli を呼ぶ）。

### ビルド手順（系統C・実バンドル時）

```sh
# 前提: tauri-cli（未導入なら）
cargo install tauri-cli --locked

# サイドカー（pika-cli.exe）をトリプル付きで配置（src-tauri/binaries/README.md 参照）
cargo build -p pika-cli --release
cp target/release/pika-cli.exe src-tauri/binaries/pika-cli-x86_64-pc-windows-msvc.exe

# バンドル（frontend ビルド→Release→NSIS。オーバーレイで externalBin/active を有効化）
cargo tauri build --config src-tauri/tauri.bundle.conf.json
# → 生成物: src-tauri/target/release/bundle/nsis/pika_<version>_x64-setup.exe
```

## 2. ポータブル zip

```sh
pwsh installer/build-portable.ps1            # フルビルドして固める
pwsh installer/build-portable.ps1 -SkipBuild # 既ビルド済みを固めるだけ
# → 生成物: installer/dist/pika-<version>-portable.zip
```

zip 同梱物: `pika.exe` / `pika-cli.exe` / `portable.txt`（データルートを ./pika-data/ に切替）/
`THIRD_PARTY_NOTICES.txt` / `LICENSE.txt` / `はじめにお読みください.md`（= docs/install.md）。

## 同梱 OSS ライセンス

`assets/THIRD_PARTY_NOTICES`（Rust クレート＋同梱 JS〔KaTeX/Mermaid/highlight.js〕＋Tauri/wry/WebView2）を
インストーラー/zip と About 画面に同梱する（要件13）。再生成は `assets/about.toml` ＋ `cargo about`（後述）。

## 導入手順とリスク

未署名配布のため SmartScreen 警告が出る。回避手順・アンインストール・データの場所は
[`docs/install.md`](../docs/install.md) に集約する（要件13）。
