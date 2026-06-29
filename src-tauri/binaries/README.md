# src-tauri/binaries/ — externalBin サイドカー配置

`tauri.conf.json` の `bundle.externalBin: ["binaries/pika-cli"]` が参照する**サイドカー実行体**を置く。
Tauri bundler はビルド時にターゲットトリプル付きの名前を要求し、インストール時に INSTDIR へ
`pika-cli.exe` として配置する（`pika.exe` の隣＝`gui_exe_path()` がそこから `pika.exe` を解決できる）。

エクスプローラー統合（要件3.3）の登録/解除は NSIS フック（`../nsis/hooks.nsh`）が
`"$INSTDIR\pika-cli.exe" --register-shell` / `--unregister-shell` を呼ぶことで行う。
ロジックは `pika-core::explorer` に集約され、ここに置く exe はその薄いラッパ（`crates/pika-cli`）。

## バンドル前の配置手順（系統C・実バンドル時）

```sh
# 1. CLI を Release ビルド
cargo build -p pika-cli --release        # → target/release/pika-cli.exe

# 2. ターゲットトリプル付きでここへコピー（x64 固定）
cp target/release/pika-cli.exe src-tauri/binaries/pika-cli-x86_64-pc-windows-msvc.exe

# 3. バンドル（tauri-cli が必要: cargo install tauri-cli）
#    externalBin はバンドル専用オーバーレイで有効化する（base に置くと dev ゲートが壊れるため）。
cargo tauri build --config src-tauri/tauri.bundle.conf.json   # → NSIS インストーラー生成
```

`externalBin` を base の `tauri.conf.json` ではなく `src-tauri/tauri.bundle.conf.json` に置く理由:
externalBin はビルド時にサイドカー実体の存在を検証するため、base に書くと実体が無い状態の
`cargo build` / `cargo test`（dev ゲート）が `resource path ... doesn't exist` で失敗する。

`pika-cli-x86_64-pc-windows-msvc.exe` は生成物のため git 管理外（`.gitignore`）。
本 README と空ディレクトリ維持のみコミットする。
