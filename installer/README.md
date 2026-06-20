# installer/ — Tauri bundler 設定（骨格）

pika の配布物（要件13・design doc 9章）をここで管理する。本ディレクトリは sprint 7 で実装する。

- ユーザー単位インストーラー（管理者不要）＋ポータブル zip（Tauri bundler / NSIS）
- エクスプローラー統合（`HKCU\Software\Classes`・ポータブルは非登録）
- アンインストール時 snapshots はユーザー選択（既定残す）のカスタムページ
- 同梱 OSS ライセンス文（`assets/THIRD_PARTY_NOTICES`）を installer/zip と About に同梱
- コード署名なし・自動更新なし（CLAUDE.md 確定判断）

実バンドル設定（tauri.conf.json の `bundle` セクション・NSIS テンプレート）と
ポータブル zip 生成スクリプトは sprint 7 で追加する。
