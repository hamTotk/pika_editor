# 引き継ぎ: プレビュー別 WebView の実結線（中心体験の核）

> このファイルは新セッション用の自己完結プロンプト。**まず本ファイル全体を読み、要点を自分で再確認してから**着手すること（下の事実は前セッションの調査結果。コードで裏取りしてから進める）。

## ゴール
pika（Tauri 版）の **プレビューがライブ UI で全く描画されない**問題を、設計の正典どおりに実結線して直す。プレビューは中心体験（AI 出力の確認・差分レビュー）の核。**確実に・安全に**直す。

- md プレビュー（系統A：同梱信頼 JS で Mermaid/KaTeX/highlight 描画）
- HTML プレビュー（系統B：文書 JS 完全無効）
- 権限ゼロの別 WebView（Tauri API 到達不能）＋ custom protocol 直配信＋CSP ヘッダ強制

## リポジトリ／起動の前提（重要な落とし穴あり）
- ルート: `C:\dev\pika_editor`、ブランチ `main`、起点 HEAD ＝ `56dbc03`（プレビュー未着手）。前セッションで F-027/F-028（軽微 UI バグ）修正済み。
- 構成: frontend `src/`（vanilla TS + Vite）／backend `src-tauri/`（Rust）／コア `crates/pika-core/`。
- **cargo が bash の PATH に無い**: 各コマンド前に `export PATH="$USERPROFILE/.cargo/bin:$PATH"`。
- **debug ビルドは埋め込み dist でなく devUrl `http://localhost:5173` を見にいく**。先に `npm run dev`（Vite）をバックグラウンド起動しないと `ERR_CONNECTION_REFUSED` で真っ白。出荷資産で見るなら release ビルド。
- GUI ビルド: `cargo build -p pika-app --bin pika`（exe = `target/debug/pika.exe`）。frontend: `npm run build`（dist 更新）／`npm run typecheck`。
- 起動はデタッチ可（単一インスタンスで引数転送）。**ビルド前に必ず `taskkill //F //IM pika.exe`**（実行中だと exe がロックされリンク失敗）。
- **PowerShell の `-ExecutionPolicy Bypass` はユーザールールで deny**。スクショ等は Python（PIL+ctypes）で行う（後述）。回避しないこと。
- データルート `%LOCALAPPDATA%\pika\`（state.json / snapshots / logs）。クリーン起動したいときは state.json を退避。退避バックアップ `_acceptance_backup_clean` が同所にある。

## 問題の核心（前セッションで特定済み・要再確認）
バックエンドは完成、**別 WebView の生成・配置・ナビゲートが丸ごと欠落**している。

### すでに実装済み（流用する）
- `src-tauri/src/preview.rs`
  - `PREVIEW_SCHEME = "pika-preview"`、custom protocol ハンドラ `handle_preview_request`（PreviewService から世代キーでサニタイズ済み HTML を引き、**CSP をレスポンスヘッダで強制**・ローカル相対参照を canonicalize＋prefix 検証・機密配信拒否）。
  - command `prepare_preview(path, mode, content, allow_external) -> PreparedPreview { url, generation, nonce, flavor, hazards }`。URL は `http://pika-preview.localhost/doc/<gen>`（Windows は custom protocol が http://<scheme>.localhost/ に解決）。**HTML 本体は invoke で返さない**。
  - `PreviewService`（managed state・世代→サニタイズ済みレスポンス保持・直近1世代のみ）。
- `src-tauri/src/main.rs`: `register_uri_scheme_protocol(PREVIEW_SCHEME, handle_preview_request)`（49行）・`prepare_preview` を invoke_handler 登録（64行）・`PreviewService` を manage（94行）。
- frontend `src/preview/index.ts`: `buildPreview()`（prepare_preview を呼ぶ）・`buildTrustedJsInit(nonce)`（Mermaid `securityLevel:'strict'`／KaTeX `trust:false,strict:true`／highlight の per-block 描画・約1秒タイムアウト・失敗件数集計の**注入用 JS 文字列**）・`parsePreviewFailureMessage()`・`PreviewSerializer`（世代直列化）・`resolveOccupancy(mode,diffOn)`・`previewModeForPath()`。
- frontend `src/main.ts`: `onTogglePreview()`（500行）・`renderActivePreview()`（519行）・`setPreviewState()`（557行）・`applyOccupancy()`（511行）・`previewHost()`（106行）。

### 欠落＝今回やること
1. `src-tauri/src/main.rs::run()` の `setup()` に **「preview」別 WebView を生成するコードが無い**（コメントだけ「sprint 4 で生成」と残る）。
2. frontend `src/main.ts:544-546` は URL を `#preview-host` の `data-preview-url` 属性に**置くだけ**で、WebView を生成・ナビゲートしていない（コメント「別WebView の src 設定は系統C」）。
3. 信頼 JS（`buildTrustedJsInit`）と同梱アセット（`assets/` の mermaid/katex/hljs・約3.84MiB）が**プレビュー WebView に注入・配信されていない**。
4. 別 WebView→メインの描画失敗件数通知が `postMessage` 前提のまま（別 WebView では届かない→**Tauri event へ要置換**）。

## 設計の正典（必ず従う）
- `docs/specs/2026-06-20-tauri-rewrite-architecture-design.md`
  - **6章**（最重要・セキュリティ境界）: 権限ゼロ別 WebView／custom protocol 直配信／**CSP はレスポンスヘッダで強制**（系統A/B の確定 CSP 文字列あり）／サニタイズ多層／信頼 JS の危険オプション封じ／系統A/B 切替の世代直列化。
  - **7章**: Rust/JS のレンダリング責務分界（Mermaid/KaTeX/highlight は JS・別 WebView・nonce 注入）。
  - **9章**: capability マップ（**プレビュー WebView=権限ゼロ**）。
  - **10章**: テーマ（別 WebView へ CSS 変数を渡す・`forced-colors`/DPI 追従）。
- `docs/requirements.md` 6章、`docs/ui-design.md` 8章（3モード×差分トグルの占有・占有世代）。

## 推奨アプローチ：子 WebView オーバーレイ
別案（独立フローティング窓）は UX が変わるので不可。**Tauri v2 `WebviewWindow::add_child(WebviewBuilder, LogicalPosition, LogicalSize)`** でメイン窓内にプレビュー用の子 WebView を重ねる（窓構造の作り直し不要）。`tauri.conf.json` のメイン窓ラベルは `"main"`。

### 着手前に解決すべきサブ判断（コードで確認して決める）
- **信頼 JS／アセットの配信経路**: `pika-core::render`（`prepare_markdown_preview`）が出すサニタイズ済み HTML に、`<script nonce>`（`buildTrustedJsInit` 相当）と mermaid/katex/hljs の参照が**含まれているか**を確認。含まれていなければ、custom protocol が `assets/` も配信する（例 `pika-preview://.../assets/mermaid.min.js`）よう拡張し、HTML に nonce 付き `<script>` を埋める。**HTML/JS をメイン WebView の JS ワールドに通さない**原則を厳守（信頼 JS も protocol 経由で別 WebView 内に閉じる）。CSP `script-src 'nonce-<rnd>'` と nonce を一致させる。
- **子 WebView の領域追従**: `#preview-host` の DOM 矩形に子 WebView を重ね、ウィンドウ/ペイン/スプリッタのリサイズに追従させる必要がある（ネイティブ子 WebView が DOM 領域を追う古典的課題）。frontend が矩形を backend に渡し、backend が `set_size`/`set_position` する。
- **DPI/座標系**: LogicalPosition/Logicalsize とデバイス DPI の扱いを実機で合わせる。

## 実装タスク
### backend（src-tauri）
1. `setup()` でメイン `WebviewWindow` を取得し、`add_child` で **label "preview"** の子 WebView を生成（初期 hidden・`about:blank` か空ページ）。**capability ファイルを置かない**＝権限ゼロを維持（`src-tauri/capabilities/` には `main.json` のみ。preview 用は作らないこと）。HTML 系統では JS を無効化する設定も検討（系統B 多層防御）。
2. command 追加（メイン capability に必要な permission を最小で付与）:
   - `show_preview(url, x, y, w, h)`: 子 WebView を矩形に配置・表示・`url` へナビゲート。
   - `hide_preview()`: 子 WebView を隠す。
   - `set_preview_bounds(x, y, w, h)`: 領域追従。
   - テーマ CSS 変数を子 WebView へ渡す（initialization script か protocol の HTML に注入）。
3. 必要なら custom protocol を拡張し `assets/`（mermaid/katex/hljs/CSS）を配信（パス封じ込め維持）。
4. 別 WebView→メインの失敗件数通知を **Tauri event**（例 `emit("preview-failures", count)`）に。

### frontend（src）
5. `renderActivePreview()`（main.ts:519）を改修: `buildPreview()`→URL 取得→`show_preview(url, rect)` で `#preview-host` の矩形に表示・ナビゲート。`PreviewSerializer` 世代で stale 破棄。`setPreviewState("loading"/"ready"/"error")` を維持。
6. `applyOccupancy()`／`onTogglePreview()`／`onToggleDiff()`／タブ切替で、占有に応じて `show_preview`/`hide_preview` を呼ぶ。プレビュー+差分は左右並置（ui-design 8章）。
7. `ResizeObserver`（`#preview-host`）＋ウィンドウ resize で `set_preview_bounds` を呼び追従。
8. `parsePreviewFailureMessage` 受信を Tauri event 購読へ置換し通知バー集計（要件6.2）。

## セキュリティ必須検証（系統C・実機）
- プレビュー WebView から **Tauri API へ到達不能**（`window.__TAURI_INTERNALS__` 不在・任意 invoke が失敗）。
- CSP がレスポンスヘッダで効く（系統A=`script-src 'nonce-..'`／系統B=`script-src 'none'`）。文書内 `<script>`/`on*`/`<meta refresh>` は ammonia で除去済み。
- ローカル参照の `..`/絶対パス/シンボリックリンク脱出を拒否・`.env`/`*.key` 等を配信拒否。
- Mermaid `securityLevel:'strict'`／KaTeX `trust:false,strict:true`／nonce のみ（`'unsafe-inline'` script を付けない）。

## 検証ハーネス（前セッションで作成・流用可。`C:\Users\nonpr\AppData\Local\Temp\`）
- `pika_shot2.py`: `PrintWindow(PW_RENDERFULLCONTENT)` で**メイン WebView**を遮蔽非依存に取得。**注意: 別 WebView（子 WebView）の中身は写らない可能性大**。
- `pika_shot_screen.py`: 画面領域グラブ（合成後ピクセル）。**プレビュー WebView の描画確認はこちら**を使う（force_foreground 後にグラブ。子 WebView がメイン窓内なら topmost 化は不要〜むしろ別窓を隠すので避ける）。
- `pika_click.py <sx> <sy>`: スクショ座標→画面座標へ変換してクリック（ツールバー「プレビュー」≈(485,63)・「差分」≈(568,63)。タブ列 y≈112）。
- `pika_key.py "ctrl+e"`: キーボード注入（前面化＋keybd_event）。ショートカット: **Ctrl+E プレビュー / Ctrl+Shift+D 差分 / Ctrl+F 検索 / Ctrl+H 置換 / Ctrl+\\ 分割**。
- 取得画像は Read tool で確認。必要なら PIL でクロップ拡大。
- フィクスチャ `C:\Users\nonpr\pika-accept`: `showcase.md`（GFM 表/Mermaid/KaTeX/コード）・`f004-showcase.md`（Mermaid/KaTeX/highlight 総合）・`withscript.html`（JS あり＝系統B で無効化されるべき）・`selfcontained.html`・`external-res.md`（外部リソース opt-in）・`broken-refs.md`（欠落参照）・`img/sample.png`（ローカル画像）。

## 検証ゲート（CLAUDE.md「完了の検証」）
1. `cargo test`（pika-core 決定論ゲート・exit 0）
2. `cargo build`（crates＋src-tauri・**警告エラー扱い** `warnings="deny"`・exit 0）
3. `npm run typecheck`（`tsc -p tsconfig.app.json`）
4. `cargo fmt --check`（Rust を触ったら）
5. **実機**: 上記スクショハーネスで md/HTML/Mermaid・KaTeX・highlight・ローカル画像・JS無効・領域追従・権限ゼロを目視確認。

## 進め方・規約
- 実装は **dev-generator サブエージェントに委譲**（ユーザー方針）。メイン側は誘導・実機検証・コミット判断に専念。大きいので段階分割推奨（① backend で子 WebView 生成＋show/hide/navigate の最小貫通→実機で md が出ることを確認 → ② アセット/信頼 JS で Mermaid/KaTeX/highlight → ③ 領域追従・占有・テーマ・失敗件数イベント → ④ HTML 系統B・外部 opt-in・暴走ガード）。
- コミット: 日本語・`type: 要約`・3行以内・末尾に `Claude-Session: <URL>` トレーラ。所見台帳は `docs/acceptance-findings.md`（前セッションの「Tauri 刷新後 実機検証」節に追記）。
- ワークスペース（pika-accept）にファイルを書かない／ユーザーデータを壊さない（最上位原則）。

## 完了の定義（Definition of Done）
- showcase.md / f004-showcase.md がプレビューで**実描画**（Mermaid 図・KaTeX 数式・コードハイライト・GFM 表・ローカル画像）。
- withscript.html が**文書 JS 無効**で安全描画（危険検知の通知バー導線が出る＝要件6.3）。
- プレビュー領域がペイン/ウィンドウ resize に追従、プレビュー⇔ソース⇔差分の占有切替が破綻しない。
- プレビュー WebView から Tauri API 到達不能（権限ゼロ実証）。
- 4ゲート緑＋実機目視 OK。

## 参考：今回の付随所見（本タスク外・別途）
- 開いている**差分ペインが外部変更で自動再計算されない**（再トグル/F5 で反映。`src/main.ts:567` 周辺の条件）。watcher は未読+1 を検知する。中心体験の即時性に関わるが本タスクとは独立。
- 検索/置換バー UI も系統C 繰り越し（`src/main.ts:983`「検索バー実体は系統C」）。本タスク外。
