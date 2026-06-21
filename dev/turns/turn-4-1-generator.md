# turn-4-1 generator report — sprint 4 プレビュー（権限ゼロ別WebView・最重要セキュリティ境界）

sprint_id=4 / iteration=1 / feedback=null（初回ターン）

## sprint goal（再注入）

プレビュー（全Web化の最重要セキュリティ境界）を実装する。権限ゼロの別WebView＋custom protocol
（pika-preview://）直配信で未信頼文書をアプリシェルから物理隔離し、comrak(unsafe_)→ammonia の最終段
サニタイズと CSP レスポンスヘッダ強制を pika-core で固める。系統A（Markdown/差分/SVG・信頼JS）/系統B
（HTML・JS無効）の切替・Mermaid/KaTeX/highlight の危険オプション封じ・双方向スクロール同期・プレビュー内検索を
配線し、design doc 15章-3 の権限ゼロ隔離を本番経路で再確認する。

設計方針（既存スプリントのパターン踏襲）: **セキュリティ判定ロジックは全て pika-core の純粋関数に置き
cargo test で固め**、src-tauri/frontend は薄い配線に徹する（design doc 3章/4章/12章）。実機・実描画・
別WebView 隔離の実証は系統C（design doc 15章-3 が明示的に「系統C」）。

## 変更ファイル一覧

### pika-core（決定論ロジック・cargo test 対象）
- `crates/pika-core/src/render/mod.rs`（新規）— prepare_markdown_preview/prepare_html_preview・PreviewResponse・各層 re-export
- `crates/pika-core/src/render/sanitize.rs`（新規）— comrak(unsafe_)→ammonia 最終段サニタイズ・系統A/B・SVG サブセット
- `crates/pika-core/src/render/csp.rs`（新規）— CSP レスポンスヘッダ組立・nonce 生成・オプトイン緩和（img/font のみ）
- `crates/pika-core/src/render/path.rs`（新規）— ローカル参照封じ込め（../絶対/シンボリックリンク/機密拒否）
- `crates/pika-core/src/render/guard.rs`（新規）— 暴走ガード（画像6000万px/SVG8000万px・5万要素/HTML10秒/長行）
- `crates/pika-core/src/lib.rs` — `pub mod render;` 追加
- `crates/pika-core/Cargo.toml` — comrak/ammonia/getrandom 依存追加
- `Cargo.toml`（workspace）— comrak="0.29"/ammonia="4"/getrandom="0.2" 追加

### src-tauri（薄い配線・cargo build 対象）
- `src-tauri/src/preview.rs`（新規）— PreviewService（世代キー保持）・prepare_preview コマンド・
  custom protocol ハンドラ（/doc/<gen> 直配信・/local/<gen>/<相対> 封じ込め配信）・scan_html_hazards
- `src-tauri/src/main.rs` — `mod preview;`・`register_uri_scheme_protocol("pika-preview", ...)`・
  prepare_preview/scan_html_hazards を invoke_handler へ・PreviewService を manage

### frontend（型チェック対象）
- `src/preview/index.ts`（実装）— 3モード×差分トグル直交占有・系統A/B 切替の世代直列化 PreviewSerializer・
  信頼 JS 注入スクリプト生成 buildTrustedJsInit・系統判定・危険検知ラッパ
- `src/ipc.ts` — preparePreview/scanHtmlHazards バインディング＋型
- `src/main.ts` — プレビュー切替ボタン・占有適用（差分トグルと直交）・renderActivePreview
- `src/index.html` — プレビュー切替ボタン・プレビュー占有領域（preview-host）

### docs（系統C 台帳）
- `docs/acceptance.md` — TE 節（sprint 4 プレビュー必達 Open・TE1〜TE8）を新設
- `docs/acceptance-findings.md` — T-007（プレビュー・決定論側 40 件＋配線＋系統C 限定）を追記

## must criteria の実装状況

| # | must criteria | 状況 | 検証手段 |
|---|---|---|---|
| 1 | サニタイズ多層（comrak unsafe_→ammonia・script/on*/javascript:/scriptable data:/iframe/object/embed/base/meta 除去・id/name 制限〔DOM clobbering〕・SVG サブセット〔script/foreignObject/on*/xlink:href javascript: 禁止〕） | **実装済** | cargo test（render::sanitize 17 件。raw HTML 内 script の最終段除去含む） |
| 2 | CSP レスポンスヘッダ強制（系統A=script-src 'nonce-rnd'／系統B=script-src 'none'・meta は ammonia 除去・オプトイン緩和は img/font のみ・script/connect/object 緩めず・既定はオフに戻る） | **実装済** | cargo test（render::csp 6 件）＋配線（preview::document_response がヘッダ付与） |
| 3 | 権限ゼロ別WebView＋custom protocol 直配信（HTML を invoke で返さず protocol 直配信・canonicalize+prefix で ../絶対/シンボリックリンク脱出拒否・機密配信拒否） | **実装済**（決定論側）＋**系統C**（隔離実証） | cargo test（render::path 9 件）＋配線（prepare_preview は URL のみ返す・local_resource_response が canonicalize+prefix＋解決後機密再判定） |
| 4 | 系統A/B 切替の直列化（世代カウンタ＋load 完了待ち＋破棄順序・JS 有効/無効を別WebView 設定で切替） | **実装済** | npm typecheck（PreviewSerializer・PreviewService.generation）。実描画切替は系統C（TE5） |
| 5 | 信頼 JS の危険オプション封じ（Mermaid securityLevel:'strict'・KaTeX trust:false/strict:true/maxExpand・highlight.js 同梱・nonce のみ・per-block 描画＋約1秒タイムアウト＋元コード復帰＋失敗件数通知） | **実装済** | npm typecheck（buildTrustedJsInit）。実注入/実描画は系統C（TE6） |
| 6 | 暴走ガード（画像6000万px・SVG8000万px/5万要素・HTML10秒を入力段で計測・超過は配信せず通知誘導） | **実装済** | cargo test（render::guard 8 件・寸法乗算 saturating） |
| 7 | cargo test PASS・cargo build・frontend 型チェック exit 0 | **達成** | 下記「自己実行 verify」 |
| 8 | design doc 15章-3 権限ゼロ隔離の本番経路再確認（必達・系統C） | **系統C 記録** | acceptance.md TE2（必達）・findings T-007 に記録。capabilities/main.json は windows=["main"] のみ＝preview ラベル未宣言で権限ゼロ |

## should criteria の実装状況

- 双方向スクロール同期（要件6.1）: comrak `sourcepos=true` で data-sourcepos を出力し ammonia で保持する
  土台まで実装（cargo test `sourcepos_data属性は...保持される`）。WebView 間メッセージの実同期は系統C。
- プレビュー内検索（要件5.4）: 信頼 JS 注入経路（nonce・別WebView）まで土台。実検索は系統C（design doc 15章-7）。
- 3モード×差分トグル直交（ui-design 8章）: resolveOccupancy で占有解決（プレビュー+差分=左右並置・ソース+差分=差分面）。
  npm typecheck 成立。実描画は系統C。
- リンク規則（要件6.2）: a[target] へ rel="noopener noreferrer" 強制（sanitize）。ラッパ command 経由の
  既定ブラウザ起動は sprint 5（CLI/shell-open ラッパ）と整合のため土台のみ。
- プレビューにテーマ CSS 変数（要件11.3）: 系統B は <style>/style 属性を尊重（sanitize）。CSS 変数注入は系統C。

## テスト化できなかった criteria とその理由

- **must #3 の「別WebView 権限ゼロ隔離の実証」/ #8（必達）**: 別WebView から invoke/__TAURI_INTERNALS__ が
  到達不能であることの確認は **Windows 実機 Release が必須**（GUI・WebView2 実体）。spec/sprints とも
  「系統C（docs/acceptance）」と明示。決定論側（capability マップが preview を含まない＝構造的に権限ゼロ）と
  配線（protocol/コマンド/サニタイズ/CSP/封じ込めロジック）まで実装し、TE2（必達）として findings T-007 に記録。
- **must #4/#5 の実描画・実注入・実切替**: 別WebView の実生成/ナビゲート・Mermaid/KaTeX/highlight の実描画は
  GUI 実機。世代直列化・注入スクリプト生成・占有解決の純粋部分は型チェック成立まで。系統C（TE5/TE6）へ記録。
- **must #1/#2/#6 の決定論部分は全て cargo test 化済み**（sanitize/csp/guard）。実描画での実効のみ系統C（TE3/TE4/TE7）。
- パス封じ込めの canonicalize+prefix は実 FS 依存のため、FS 非依存の判定（resolve_local_ref/confine_under）を
  cargo test 化し、canonicalize 実行は src-tauri 側（cargo build 対象）に置く設計分割とした（TE8）。

## 自己実行した verify の結果

- `cargo test`（sprints.json verify[0]）: **PASS**
  - pika-core: 133 passed / 0 failed（render 41 件〔sanitize 18・csp 6・path 9・guard 8〕新規追加。従来 92 件含む）
  - pika-app（src-tauri）: 3 passed（preview 配線テスト）
  - pika-cli: 4 passed / bin pika: 3 passed / doc-tests: 0
- `cargo build`（sprints.json verify[1]）: **PASS**（exit 0・workspace lints `warnings = "deny"` でも警告ゼロ）
- frontend 型チェック `npm run typecheck`（系統A補・CLAUDE.md の完了検証）: **PASS**
- `npm run build`（tsc + vite build・従来スプリント踏襲）: **PASS**（built in ~0.9s）

> 合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

## 補足（評価者向け）

- テストの緩和・スキップ・無効化は行っていない。ammonia 4 の制約（`style` が既定 clean_content_tags と
  両立しない）は API 仕様であり、系統B のみ clean_content_tags を再設定して <style> を許可（要件6.3 文書スタイル尊重）。
  この扱いは sanitize.rs にコメントで根拠を記載。
- data: URL は url_schemes に含めず**全除去**（scriptable data: の除去を最優先・要件6.2）。ラスタ画像も data: は
  不可だが、ローカル画像は custom protocol(/local/) 経由で封じ込め配信する設計のため機能後退しない。
- 機密ファイル配信拒否は多層: resolve_local_ref（参照名）＋ local_resource_response（canonicalize 後の実体名）の二重。

DONE: C:/dev/pika_editor/dev/turns/turn-4-1-generator.md
