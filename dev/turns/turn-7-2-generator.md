# turn 7-2 generator report — sprint 7（a11y キーボード操作性・ショートカット配線・保存整合の是正）

## 概要

sprint 7 の iteration 2。前ターン eval（turn-7-1・score 84・d_accept=0 でブロック）の feedback_structured を
**high 2 件 → medium 5 件 → low 一部** の順に是正した。中心は「frontend がキーボードのみで中心体験の起点に
到達できない」ブロッカー（d_accept=0 の原因）と、core 側に実装済みだが frontend に未配線だった主要ショートカット。
決定論ゲート（pika-core ロジック）は本是正で非回帰（frontend 配線とデータ整合の結線が中心）。

全 verify gate exit 0: `cargo test` 317（pika-core）/11（pika-cli）/5（pika-app）PASS・`cargo build`（警告エラー扱い）・
`cargo fmt --check` clean・`npm run typecheck`（noUnusedLocals/noUnusedParameters 込み）成立。

## 変更ファイル一覧

### frontend（a11y キーボード・ショートカット配線・保存整合）
- `src/ui/tree.ts` — **ファイルツリーのキーボード操作を本実装**（eval high#1）。roving tabindex（ツリー内で常に
  1 つだけ tabIndex=0）・↑/↓/Home/End で treeitem 移動・**Enter/Space で openFile 発火**・ディレクトリ treeitem に
  **aria-expanded**・状態マークを aria-label テキスト化。
- `src/shortcuts.ts`（新規）— `pika-core::shortcuts::resolve` の**1:1 写し**（`resolveShortcut`・`modsOf`・`normalizeKey`）。
  パリティ契約をヘッダに明記（Rust 側が正本・cargo test 11 件が決定論ゲート）。
- `src/main.ts` — (1) **keydown を resolve ベースのディスパッチへ置換**（`currentFocus`→`resolveShortcut`→`dispatchAction`。
  eval high#2: Ctrl+O/Shift+O/S/W/F/H/\\/Shift+E/Shift+D/F8系/Ctrl+Enter 系が発火）。(2) **onSave を save_document へ**寄せ、
  openFile を **open_document** に変更してエンコーディング/BOM をタブに保持（eval medium: Shift_JIS 暗黙 UTF-8 化の解消・
  表現不能文字で保存中断）。(3) **switchFolder を三択化**（保存して切替/破棄して切替/キャンセル・対象名列挙。eval medium）。
  (4) onCloseActiveTab（Ctrl+W）・onOpenFile（Ctrl+O）。(5) **announce で差分件数を sr-live へ**（eval medium ARIA）。
  (6) Empty 3分岐文言（emptyMessage で no-folder/all-consumed 結線・eval low）。
- `src/ui/tabs.ts` — **tablist の roving tabindex＋←/→/Home/End 移動**・バッジ状態を aria-label テキスト化（eval medium ARIA）。
- `src/a11y/index.ts` — **announce(message)** 追加（視覚的に隠した polite ライブ領域 #sr-live へ差分件数を読ませる・eval medium）。
- `src/index.html` — `#sr-live`（`.sr-only` role=status aria-live=polite）を追加。
- `src/styles/app.css` — `.sr-only` ユーティリティ・treeitem/tab の `:focus-visible`（キーボードフォーカス可視化）。

### src-tauri（並行書込の直列化）
- `src-tauri/src/diagnostic.rs` — `log_with_min` 全体を **`OnceLock<Mutex<()>>` で直列化**（eval medium data: 並行書込で
  世代ずれ/行取りこぼし/5MB超過 append を防ぐ）。毒ロックは into_inner で続行（診断は副次・固まらない）。

### docs（系統C 整備）
- `docs/acceptance-findings.md` — **T-011**（本 iteration の是正根拠・系統C 残項目・cargo audit/永続化の環境制約記録）追記。
- `docs/acceptance.md` — TG2/TG8 注記を T-011 反映へ更新、**TG11**（ツリーのキーボード操作）・**TG12**（保存のエンコーディング
  維持・表現不能中断）を追加（各 requirements/design doc 節番号併記）。

## 前ターン feedback_structured への対応

### high
- **[a11y/キーボード操作（frontend-ui critical）]** ✅ `tree.ts` に roving tabindex・↑/↓/Home/End・**Enter/Space で
  openFile**・tree host への Tab 経路（先頭 treeitem tabIndex=0）・ディレクトリ aria-expanded を実装。マウスなしで
  「開く→プレビュー→差分→確認済み」の**起点に到達できる**ようになった（前ターンの d_accept=0 ブロッカーの解消）。
- **[ショートカット配線]** ✅ `pika-core::shortcuts::resolve` の写し `shortcuts.ts` を新設し、`main.ts` の独自インライン
  keydown を resolve ベースのディスパッチへ置換。dead code 化していた Ctrl+O/S/W/F/H/Ctrl+Enter 系がキーボードから発火する。

### medium
- **[エラー回復・データ整合（ux）]** ✅ onSave を save_document 経路へ。openFile も open_document でエンコーディング検出し
  タブ保持→保存時に渡す。Shift_JIS 等の暗黙 UTF-8 化を防ぎ、表現不能文字で保存中断し ［UTF-8で保存/キャンセル］提示。
- **[ログ永行/並行書込（data）]** ✅ diagnostic.rs の log_with_min を専用 Mutex で直列化。
- **[破壊的操作の確認（ux）]** ✅ switchFolder を対象名列挙＋三択（保存/破棄/キャンセル）へ。「保存して切替」は失敗時に切替中止。
- **[ARIA 属性/ライブ通知]** ✅ (1) ディレクトリ aria-expanded。(2) #sr-live ライブ領域へ差分件数 announce。
  (3) tablist roving tabindex＋矢印移動。

### low（記録に留める／一部対応）
- **[依存/CVE 監査（security）]** cargo audit は本環境に cargo-audit 未導入で未実行（sprint 7 verify 配列は cargo test/build
  のみ・テスト緩和ではない環境制約）。T-011 に記録しリリース前（系統C）に advisory DB 照合する旨を明記。
- **[スナップショット永続化（data・非回帰）]** インメモリ HashMap 繰り越し（sprint 3 からの意図的繰り越し・sprint 7 acceptance
  外）。T-011 に「リリース前に index.json+content-addressed object 復元の永続化が要る」と記録。
- **[空状態 3 分岐の DOM 未結線（ux）]** emptyMessage を no-folder/all-consumed に結線（部分対応）。完全な 3分岐 DOM/CTA は系統C TG4。
- **[診断ログ path のユーザー名弱開示 / changed_files target 混入]** 記録に留める（前者は将来データルート相対化・後者は本ターンの
  変更はソースのみで target/ を編集していない）。

## must / should criteria 実装状況（sprint 7・iteration 2 時点）

### must（前ターンで未充足とされた 2 件を是正）
- **ARIA 全Web再構築（frontend・design doc 17章）** — ✅ 是正完了。ツリー treeitem のキーボード操作性（roving tabindex・
  矢印・Enter/Space・aria-expanded・状態 aria-label）を本実装し、req 11.4/11.5「マウスなしで中心フロー完走」の起点到達を
  満たした。F6/Shift+F6・tablist 矢印移動・通知 aria-live・ステータス差分件数の sr-live announce も結線。実読み上げ/実遷移は
  系統C（TG1/TG2/TG11）。
- **主要ショートカット表（should だが d_review に効いていた）** — ✅ frontend へ配線完了。判定は core resolve（cargo test 11 件）、
  発火は dispatchAction。実発火は系統C（TG8）。
- 他 must（通知バーキュー/5状態/非テキスト縮退/診断ログ/docs 整備）— iteration 1 で充足済み・本 iteration で非回帰。

### should
- ✅ 主要ショートカット表のフロント配線（上記）。✅ forced-colors（既存維持）。
- 部分: Tauri bundler 配布（実インストーラー）はリリース準備で系統C（TG10）。

## テスト化できなかった criteria とその理由

- **キーボード実フォーカス遷移・ショートカット実発火・読み上げ・三択確認の実挙動**: DOM/WebView2/ナレーター/実 FS が要るため
  系統C（acceptance.md TG1/TG2/TG8/TG11/TG12）。判定ロジック（shortcuts::resolve・encoding 往復・unmappable・rotation 計画）は
  cargo test で観測可能（決定論ゲート）。frontend の shortcuts.ts は Rust resolve の写しで、本リポジトリに JS テストランナーが
  無いため typecheck（コンパイル/型成立）を最低保証とする（spec 検証戦略 系統A補）。パリティ契約をヘッダに明記して drift を抑える。
- **診断ログの並行書込直列化**: ロックは src-tauri（系統A補・cargo build 成立を保証）。純粋ロジック plan_rotation は pika-core で
  既にテスト済み（eval も「plan_rotation の純粋ロジック自体は正しい」と認定）。

## 自己実行した verify 結果

- `cargo test`（全 workspace）: pika-core **317** / pika-cli **11** / pika-app **5** passed・failed 0。
- `cargo build`（crates＋src-tauri・警告エラー扱い）: **exit 0**。
- `cargo fmt --check`: **clean**。
- `npm run typecheck`（tsc -p tsconfig.app.json・noUnusedLocals/Parameters 込み）: **exit 0**。

（合否判定の正本は run-dev が別途実行する結果。上記は自己確認。）

DONE: C:/dev/pika_editor/dev/turns/turn-7-2-generator.md
