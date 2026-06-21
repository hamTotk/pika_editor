# turn-4-2 generator report — sprint 4 プレビュー（権限ゼロ別WebView）修正ターン

sprint_id=4 / iteration=2 / feedback=dev/turns/turn-4-1-eval.json（score=88・全足切りクリアだが score<90 で修正ターン）

## sprint goal（再注入）

プレビュー（全Web化の最重要セキュリティ境界）を実装する。権限ゼロの別WebView＋custom protocol
（pika-preview://）直配信で未信頼文書をアプリシェルから物理隔離し、comrak(unsafe_)→ammonia の最終段
サニタイズと CSP レスポンスヘッダ強制を pika-core で固める。系統A/B 切替・Mermaid/KaTeX/highlight の
危険オプション封じ・双方向スクロール同期・プレビュー内検索を配線し、design doc 15章-3 の権限ゼロ隔離を
本番経路で再確認する。

本ターンは前イテレーションで全 must を充足済み（d_verify=100/d_accept=100/d_review=100）で score=88 だったため、
**feedback_structured の high(4件)→medium(6件)→low(6件) を high から順に潰して品質を上げる**ことに専念した。

## 対応した feedback（high → medium → low）

### HIGH（4件・すべて対応）

1. **暴走ガード未結線（performance+ux 収束・must#6）** — 解消。
   - pika-core::render::guard に `check_image_bytes`（PNG/GIF/JPEG/BMP/WebP のヘッダ寸法をデコード前に読む）と
     `check_svg_bytes`（要素数計数＋viewBox/属性からの推定px）を追加。dead だった guard を**配信経路へ結線**。
   - `src-tauri preview.rs::local_resource_response` が配信前に `guard_local_resource` →pika-core guard を呼び、
     6000万px超画像/5万要素超SVG/8000万px超SVG を **413（PAYLOAD_TOO_LARGE）で配信拒否**する（WebView 任せにしない）。
   - cargo test: pika-core 側に PNG/GIF/JPEG ヘッダ寸法・SVG 要素数/推定px の判定テスト（8件）、
     src-tauri 側に配線テスト `ローカル配信の暴走ガードが結線されている`（巨大→Block/通常→Allow/CSS→Allow）を追加。

2. **CSP ディレクティブインジェクション（security・csp.rs）** — 解消。
   - pika-core::render::csp に `validate_allow_hosts`（https:// のみ・ホスト名/ポート文字種限定・空白/`;`/クォート/
     `*`/改行/`,`/`\`・パス/クエリ拒否）を新設。
   - `prepare_markdown_preview`/`prepare_html_preview` が **CSP 組立前に検証し、1 つでも不正なら外部許可を全破棄
     （fail-closed＝既定遮断へ倒す `sanitize_allow`）**。さらに `build_csp` 側でも防御的に不正ホストを連結しない（多層）。
   - cargo test: `https://evil.com; script-src *` 等が CSP に漏れず nonce 限定が保たれること・非https/空白/ワイルドカード/
     パス付き/不正ポートの拒否・正常ホストの img/font 反映を観測（csp 6件＋mod 2件）。

3. **信頼JS失敗件数の通知が片側配線（frontend-ui+ux 収束・must#5）** — 受信導線を追加。
   - メインWebView に `window.message` リスナを追加し `parsePreviewFailureMessage`（型検証・count>0 のみ）で受理→通知バー集計。
   - **別WebView は独立 WebView ゆえ本番は window.parent 不達**。Tauri event/IPC 経路への置換が要る旨を
     コード／findings／acceptance.md TE6 に明記（系統C T-007/TE6 で追跡）。`scan_html_hazards` の独立 command は廃し
     hazards を prepare_preview の戻りに同梱（下記 medium と統合）。

4. **非同期中の二重送信防止欠落（frontend-ui）** — 解消。
   - `withBusy`（busy フラグ＋即時 disabled）で保存/確認/一括確認/巻き戻しを in-flight 抑止。連打で
     confirm_all/rollback_file が並行発火しデータ操作が重複するのを防ぐ（最上位原則「データを失わない」の UI ガード）。
   - `refreshTabs` も busy 中は退避系ボタンを無効に保ち、内部 refresh での再有効化による抜け道を塞ぐ。

### MEDIUM（6件）

- **has_meta_refresh 通知握り潰し（ux）** — 解消。renderActivePreview が `ready.hazards.has_meta_refresh` を通知。
- **プレビューペインの状態網羅＋CSS占有未整備（frontend-ui）** — 解消。`#preview-host` に grid-row:2 占有 CSS と
  data-state（loading/ready/error/empty）のプレースホルダ CSS を追加。renderActivePreview が setPreviewState を呼ぶ。
- **HTML プレビューの IPC 二重転送＋全文lowercase（performance）** — 解消。hazards を prepare_preview の戻りに同梱し
  scan_html_hazards command を廃止＝同じ content の二重 invoke と二重 lowercase 走査を解消（1 invoke 化）。
- **すべて確認済みスキップ時の次の一手欠落（ux）** — 解消。skipped>0 で「F5 再同期して再確認」を notify。
- **style-src unsafe-inline の見直し余地（security）** — sprint7（forced-colors で style 増）で nonce 寄せ再評価する旨は
  既存コメントに記載済み。criterion は style-src を制約せず必須でないため本ターンは据え置き（feedback 指針どおり）。
- **.pika-block-error 弁別スタイルの配信CSS確認（frontend-ui）** — 別WebView へ配信する完全 HTML 文書ラッパ（head/CSS/
  nonce script 注入）は系統C の別WebView 実アタッチで組む段（本スプリントは body フラグメント＋CSP まで）。
  findings に「配信 CSS の色非依存弁別スタイルは系統C結線時に確認」と追跡を残した（据え置き＝設計分割どおり）。

### LOW（6件）

- **scan_html_hazards は防御層でなく UX補助（security）** — `scan_hazards` のドキュメントコメントに「検知＝防御に昇格させない
  （実防御は ammonia+CSP）」を明記。
- document_response 本体フルコピー / markdown フル再変換 / PreviewService 世代束縛 / is_sensitive 単一源維持 /
  preview-host role未指定・トグル代替割当 — いずれも YAGNI・系統C・sprint7 範囲（A5/ショートカット表）として据え置き
  （feedback も「現段階の最適化は YAGNI」「sprint7」と明記）。is_sensitive 単一源は現状維持で良好点。

## 変更ファイル一覧

### pika-core（決定論ロジック・cargo test 対象）
- `crates/pika-core/src/render/csp.rs` — `validate_allow_hosts`/`validate_one_host` 追加・build_csp の防御的再検証・テスト追加
- `crates/pika-core/src/render/guard.rs` — `check_image_bytes`/`check_svg_bytes`＋画像ヘッダ寸法/SVG 計数ヘルパ・テスト追加
- `crates/pika-core/src/render/mod.rs` — 新 API の re-export・prepare_*_preview の fail-closed 検証（`sanitize_allow`）・テスト追加

### src-tauri（薄い配線・cargo build 対象）
- `src-tauri/src/preview.rs` — local_resource_response への暴走ガード結線（`guard_local_resource`・413）・
  PreparedPreview に hazards 同梱・`scan_hazards` 純粋化・`scan_html_hazards` command 廃止・配線テスト追加
- `src-tauri/src/main.rs` — invoke_handler から `scan_html_hazards` を除去

### frontend（型チェック対象）
- `src/preview/index.ts` — `detectHtmlHazards`/`scanHtmlHazards` 廃止・PreviewReady に hazards・`parsePreviewFailureMessage` 追加
- `src/ipc.ts` — PreparedPreview に hazards・HtmlHazards 移設・scanHtmlHazards バインディング除去
- `src/main.ts` — withBusy 二重送信防止・hazards 統合（has_meta_refresh 含む）・失敗件数 message リスナ・
  プレビュー 5状態 setPreviewState・confirm_all スキップ時 F5 案内
- `src/styles/app.css` — #preview-host のグリッド占有＋5状態プレースホルダ CSS

### docs（系統C 台帳）
- `docs/acceptance.md` — TE4（CSP host 検証）・TE6（失敗件数受信導線・event 経路置換）・TE7（暴走ガード本番結線）更新
- `docs/acceptance-findings.md` — T-007 に iteration2 修正（暴走ガード結線・CSP host 検証・失敗件数受信・二重送信防止・medium）追記

## must criteria の実装状況（全 must 維持・前ターンから後退なし）

| # | must criteria | 状況 | 検証手段 |
|---|---|---|---|
| 1 | サニタイズ多層 | 維持 | cargo test（render::sanitize） |
| 2 | CSP レスポンスヘッダ強制（緩和は img/font のみ・既定遮断復帰） | 維持＋**インジェクション対策強化** | cargo test（csp・validate_allow_hosts） |
| 3 | 権限ゼロ別WebView＋custom protocol 直配信・パス封じ込め・機密拒否 | 維持 | cargo test（render::path）＋配線 |
| 4 | 系統A/B 切替の直列化 | 維持 | npm typecheck（PreviewSerializer）・系統C TE5 |
| 5 | 信頼JS 危険オプション封じ＋**失敗件数通知（受信導線追加）** | 強化 | npm typecheck（buildTrustedJsInit/parsePreviewFailureMessage）・系統C TE6 |
| 6 | 暴走ガード（入力段計測・**本番経路へ結線**） | **強化（dead 解消）** | cargo test（guard・preview 配線テスト）・系統C TE7 |
| 7 | cargo test PASS・cargo build・frontend 型チェック exit 0 | 達成 | 下記 verify |
| 8 | 権限ゼロ隔離の本番経路再確認（必達・系統C） | 系統C 記録 | acceptance.md TE2・findings T-007 |

## テスト化できなかった criteria とその理由

- must#3/#8 の別WebView 権限ゼロ隔離の実証・#4/#5 の実描画/実注入・暴走ガードの実描画は **Windows 実機 Release が必須**
  （GUI・WebView2 実体）。spec/sprints とも系統C と明示。本ターンの追加分も決定論側（guard 寸法読取・CSP host 検証・
  hazards 検知・受信メッセージ検証）と配線（413 結線・message リスナ）まで cargo test/typecheck で固め、
  実挙動は系統C（TE6/TE7・T-007）へ記録。
- 失敗件数の本番経路（別WebView→本体）は window.parent 不達のため Tauri event/IPC への置換が要り、系統C結線で実施（TE6 追跡）。
- frontend の純粋関数（parsePreviewFailureMessage 等）は本プロジェクトに JS テストランナーが無く、spec の検証戦略どおり
  npm typecheck（系統A補）で型成立を最低保証にする。

## 自己実行した verify の結果

- `cargo test`（sprints.json verify[0]）: **PASS**
  - pika-core: 149 passed / 0 failed（render 強化＝csp +6・guard +8・mod +2 で前回 133→149）
  - pika-app（src-tauri）: 5 passed（preview 配線テスト＝guard 結線・hazards 同梱を追加し前回 3→5）
  - pika-cli: 4 passed / bin pika: 5 passed / doc-tests: 0
- `cargo build`（sprints.json verify[1]）: **PASS**（exit 0・workspace lints warnings=deny で警告ゼロ）
- `cargo fmt --check`: **PASS**（CI フォーマット担保。整形を適用済み）
- frontend `npm run typecheck`（系統A補）: **PASS**（exit 0）
- `npm run build`（tsc + vite build）: **PASS**

> 合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

## 補足（評価者向け）

- テストの緩和・スキップ・無効化は行っていない。既存テストの期待値も変更していない（新規追加のみ）。
- `scan_html_hazards` command 廃止は機能後退ではなく、同じ危険検知（hazards）を prepare_preview の戻りに統合して
  IPC 二重転送を解消したもの（要件6.3 の通知導線は維持・むしろ has_meta_refresh の通知漏れを解消）。
- 暴走ガードの画像寸法読取はヘッダのみ（フルデコードしない）。判定不能フォーマットは安全側で Allow（誤ブロックより通す・
  実消費は WebView 側で頭打ち）とし、SVG は確実な要素数を主判定にして過小評価を避ける設計にした。

DONE: C:/dev/pika_editor/dev/turns/turn-4-2-generator.md
