# エクスプローラー関連付け用 種別アイコン（要件3.3）

pika を既定アプリにしたファイルが **Windows エクスプローラー上で表示するアイコン**
（レジストリ `DefaultIcon`）を、ファイル種別ごとに別デザインで用意する。
VSCode 風の「文書（ページ）＋折れ角＋種別ラベル帯」。配色は pika テーマ
（`src/styles/tokens.css`）に準拠。

> メモ: ファイル種別アイコンは **エクスプローラー専用**。アプリ内ツリーの種別アイコンは
> 別系統（フロントエンドのモノトーン線アイコン）で、こことは独立している。

## ファイル

| ファイル | 役割 |
| --- | --- |
| `generate.py` | アイコン生成スクリプト（開発ツール。要 Pillow） |
| `md.ico` / `html.ico` | 生成済み多解像度アイコン（**コミット対象**。ビルドに Python は不要） |
| `preview.png` | `--preview` 時のみ生成される確認用画像（コミット不要） |

`.ico` をコミット済みにしているため、通常のビルド／インストーラ生成では Python は要らない。
アイコンを**足す・変える**ときだけ `generate.py` を実行する。

## アイコンの配信経路

`src-tauri/tauri.bundle.conf.json` の `bundle.resources` でこれらの `.ico` を NSIS
インストーラに同梱し、`pika.exe` の隣（INSTDIR）へ展開する。インストール後フックが
`pika-cli.exe --register-shell` を呼び、`pika-cli` が exe 隣の `.ico` を見つけて
`DefaultIcon` に絶対パスで書き込む（`crates/pika-cli/src/main.rs` の `resolve_icon_dir`）。
`.ico` が見つからない場合（dev ビルド等）は exe のアプリアイコンへ自動フォールバックする。

ポータブル版はエクスプローラー統合を登録しない（要件3.3）ため、アイコンも使わない。

## 種類を増やす手順

1. **`generate.py` の `TYPES`** に 1 エントリ足す（`name`＝出力ファイル名・`label`・`color`・
   `font`・`scale`）。
2. `python generate.py` を実行して `.ico` を再生成する（`--preview` で見た目を確認）。
3. **`crates/pika-core/src/explorer/mod.rs` の `ASSOC_TYPES`** に同じ種別を足す
   （`progid` / `extensions` / `icon_file` ＝ 手順1の `name`.ico）。登録ロジックの単一源。
4. **`src-tauri/tauri.bundle.conf.json` の `bundle.resources`** に新しい `.ico` を足す。
5. `cargo test`（登録ロジックのテスト）／`cargo build` を通す。

`generate.py`（見た目）と `ASSOC_TYPES`（登録）は別ファイルなので、`icon_file` 名と
拡張子グループが食い違わないよう両方そろえること。

## 再生成

```sh
# このディレクトリで（要 Pillow: pip install pillow）
python generate.py            # .ico を再生成
python generate.py --preview  # preview.png も出力して見た目を確認
```
