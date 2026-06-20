# pika — Tauri フル刷新フェーズ 開発スプリント仕様（spec.md）

## 出典（source_doc）

本仕様は以下の正典ドキュメントを正として整形したものである（元ドキュメントは書き換えない／Read のみ）。
ドリフト追跡のため出典パスを冒頭に記録する。優先順位は「Tauri スタックの技術正典 ＞ 機能要件の正典 ＞ 視覚仕様の正典」。

1. `C:\dev\pika_editor\docs\specs\2026-06-20-tauri-rewrite-architecture-design.md` — **Tauri スタックの技術正典（最優先・承認済み v2）**。本フェーズが従う「載せ替え方」。本書のスプリント骨子は design doc 14章（実装順序）、各スプリント must は design doc 15章（実装前 Open 10項目）と 19章（移植チェックリスト）、初期スプリントのタスクは design doc 16章（旧 canon 改訂）に依拠する
2. `C:\dev\pika_editor\docs\requirements.md` — **機能要件の正（全14章＋各章「受け入れ基準」）**。機能の可否はここで判断。Tauri 化でも要件はそのまま引き継ぐ（design doc が上書きするのは 1章/2.2/2.3/11.5 の技術手段のみ）
3. `C:\dev\pika_editor\docs\ui-design.md` — **UI視覚仕様の正典**（配色トークン・タイポ・状態記号〔差分あり ±／新規 ◆／削除済み 取消線〕・差分3モード×差分トグル直交・ステータス右下固定・5状態）。視覚仕様は流用、実装手段の wx 直書き箇所（§6 wxBitmapBundle・§13 UIA/MSAA・§2 WebView2注入・固定px）は design doc 16章で Web 向けに改訂する
4. `C:\dev\pika_editor\CLAUDE.md` — プロジェクト指示（設計原則の優先順位・確定済み判断）
5. `C:\dev\pika_editor\dev-ui-2026-06-16\spec.md` / `dev-core-2026-06-16\spec.md` — 前フェーズ（wx 版 UI／コア層）run-dev 成果物。spec の構成（出典/設計原則/目的/中心体験/検証戦略二系統）と sprints の sprint 構造を踏襲する。**ただし wx 版コア（gtest 735 件）は本フェーズで破棄され、Rust 側でロジックを再建する**（旧コードは検証実装の参照仕様としてのみ価値を持つ）

設計原則の優先順位（CLAUDE.md / design.md 1章。design doc 1章で補助原則を改訂。スプリント設計はこれに従う）:
**データを失わない ＞ 固まらない（UIスレッド200ms超ブロック禁止・IPCラウンドトリップもコスト計上）＞ 軽い ＞ 足さない（要件14章を増やさない）＞ 速く作る**

補助原則（design doc 1章・旧「ネイティブ優先」を置換）:
- **コアは UI を知らない** — `pika-core`（Rust）は Tauri/wry/WebView を知らない。橋渡しは Tauri command/event のみ
- **未信頼コンテンツはアプリシェルから物理隔離する** — 文書由来 HTML/Markdown は権限ゼロの別 WebView で描画し Tauri API から物理分離
- **ワークスペースを汚さない** — ユーザーフォルダに pika のファイルを一切作らない

---

## 目的

Windows 11 x64 向け超軽量 Markdown/HTML エディタ「pika」を、**wxWidgets/C++ から Tauri（Rust + WebView2 + HTML/CSS/TypeScript）へフル刷新**する。
VSCode/Obsidian 同等の「全Web描画＋CSS変数トークン」由来の質感を、wx 版（9.6MB）と同等以下の軽さ（Tauri スパイク実測 8.6MB）で両立する。

機能要件は `requirements.md`（全14章）を**そのまま正典として引き継ぎ**、本フェーズが決めるのは「Tauri スタックへの載せ替え方」のみ
（design doc がスコープ）。中心体験（開く→外部変更反映→差分→確認済み）を Tauri 上で貫通させたうえで周辺機能を肉付けする。

成功条件（design doc 1章）:
- 中心体験が Tauri 上で貫通する
- 性能目標（要件2.1）を満たす。**IPC ラウンドトリップを性能予算に織り込んでなお目標内**（巨大ファイル閾値のみ実測で Web 現実値へ改訂）
- 配布サイズ 30MB 以内
- コアロジックが `cargo test` で自動検証（**旧 gtest 735 件相当の決定論ゲート再建**を目標）
- アクセシビリティ（要件11.5 を全Web向けに改訂した水準・design doc 17章）を満たす

中心体験（最優先で貫通させる縦切り。design doc 14章2）:
1. **開く** — フォルダ/ファイルを開き、ツリーに表示・タブで開く（CLI `pika <path>` / 単一インスタンス転送）
2. **外部変更を反映** — AIエージェントの編集を watcher 自前合成層で即時反映し、ツリーに差分あり（未読）マーク
3. **差分** — 前回確認時点からの累積差分を read-only unified ビューで赤/緑＋記号表示（3モード × 差分トグル直交）
4. **確認済みにする** — ベースラインを更新して差分マークを解除（退避が最後の砦）
5. 必要に応じて人間が CM6 で軽く修正して保存する（エンコーディング往復・衝突退避）

---

## 検証戦略（本フェーズの核。verify 二系統 — 補完判断1で根拠を詳述）

前フェーズの「系統A 決定論ゲート＋系統C 手動 acceptance」体裁を踏襲する。本フェーズは Tauri 化により
**(A) `cargo test` で固められる pika-core ロジックの比重を最大化**し、Tauri/WebView2 実機・実測が要る項目（design doc 15章 Open の多く）は
**(C) 手動 acceptance** へ寄せる。前フェーズと違い、Tauri scaffold 後は frontend の型チェック（系統A の一部）も verify に乗る。

### 系統A：決定論ゲート（各スプリントの **must verify**・主要品質シグナル）

UI に依存しないロジックを Tauri/wry 非依存の独立クレート `crates/pika-core` に置き、`cargo test` で検証する
（design doc 12章・4章。旧 C++ core/ の責務境界を Rust モジュールへ写す）。対象（design doc 12章テスト方針）:

- **diff** — Myers（similar）＋日本語等の語境界不成立行の**文字単位フォールバック**（unicode-segmentation）・改行混在・空ファイル・LF 正規化照合
- **watcher** — 自前合成層のイベント合成/合体・自己保存抑制（ハッシュ一致主条件・ワンショット）・オーバーフロー再同期・rename 正規化（FileId 補強）
- **snapshot** — ベースライン・退避（conflict/incoming/rollback/baseline-replace）・content-addressed object・容量GC・自己記述メタからの index 破損復元
- **encoding** — encoding_rs による BOM/UTF-8/UTF-16/Shift_JIS 判定・往復・**表現不能文字検出**（encoder unmappable 自前ハンドリング）
- **render サニタイズ** — comrak(`unsafe_`)→ammonia の最終段サニタイズ（`<script>`・`on*`・`javascript:`・`<iframe>/<object>/<embed>/<base>/<meta>` 除去・DOM clobbering 防止・SVG サブセット）・CSP 組立・暴走ガード入力段計測
- **search** — fancy-regex（後方参照・キャプチャ参照・Unicode文字クラス）の機能テスト・ReDoS タイムアウト
- **settings/state/workspace/CLI 解析** — toml 読取・不完全TOMLで直前維持・state.json version 安全側・自然順/除外・`-g` パース（ドライブレターのコロン非分割）・データルート解決

→ **must verify**: `cargo test`（pika-core。exit code 0 で合格）。「ループが進むほど合否判定が決定論側に寄る」担保。

### 系統A補：コンパイル/型ゲート（**must verify**・ユニットテスト不能部分の最低保証）

Tauri command 層・src-tauri 全体・frontend TS はユニットテストしにくい（wry/WebView2 実体・DOM）。
**コンパイル成立＋型チェック成立**をゲートにする（design doc 12章「警告をエラー扱い」）:

- `cargo build`（`crates/*`＋`src-tauri`。警告エラー扱い。exit 0＝コンパイル+リンク成功）
- frontend 型チェック（`tsc --noEmit` 相当 / `npm run build`。**正確なコマンドは scaffold 後に確定**し、確定スプリント以降の verify に載せる）

### 系統A監査：CI ゲート（**should/該当スプリント verify**）

- `cargo audit`（comrak/ammonia/Mermaid/KaTeX 等の CVE 追跡。design doc 12章。CI ゲート。**ネットワーク前提のため verify として安定実行できる段で must、それ以前は should**）

### 系統C：実機・実測の手動 acceptance（must verify に載せない・docs/acceptance.md / docs/acceptance-findings.md）

Tauri/WebView2 の実描画・実測・a11y は GUI 実機が要るため、`docs/acceptance.md`／`docs/acceptance-findings.md` の手動チェックリストへ集約する
（design doc 12章「UI 自動テストは初期版で持たず手動チェックリストで代替」）。**design doc 15章 Open の多くがここに落ちる**。
各 acceptance 項目には対応する **requirements / design doc の節番号を併記**する（後からドリフトを追跡できるように）。主な項目:

- **IPC コスト実測**（design doc 15章-1・3章）— プレビューHTML/保存/diff/巨大ファイルの転送方式が性能目標（要件2.1）内
- **別WebView 権限ゼロ隔離の実証**（design doc 15章-3・6章）— プレビュー WebView から `invoke`/`__TAURI_INTERNALS__` 経由の任意 command が**到達不能**であることを Windows 実機 Release で確認。1つでも到達したら設計やり直し
- **watcher オーバーフロー再同期**（design doc 15章-2・4章・要件7.4）— notify が ERROR_NOTIFY_ENUM_DIR 相当を拾うか／100件同時変更で取りこぼさないか
- **CM6 巨大ファイル体感**（design doc 15章-6・8章・要件2.2）— 「10MB 第1段階で編集・検索・保存が通常通り」死守可否 → 要件2.2/5.4/9.2 改訂値
- **性能ゲート**（要件2.1）— 起動0.5秒/プレビュー初回2.0秒/外部変更反映500ms/メモリ各上限/IPC ラウンドトリップ/常駐リーク（18時間ソーク・外部変更1000回・プレビュー50回更新）
- **a11y 実機**（design doc 15章-4・17章・要件11.5）— ナレーター/UIA で主要UI・別WebView 内プレビューが辿れるか／F6 フォーカス循環／forced-colors 追従
- **WebView2 不在時起動**（design doc 15章-5・18章・要件2.3 改訂）— 不在/破損時に最小ネイティブダイアログで導入案内し終了するか
- **プレビュー内検索**（design doc 15章-7・5章・要件5.4）— WebView2 Find が別WebView に効くか／自前検索スクリプトか
- requirements.md 各章「受け入れ基準」の写経（リリース前）

### use_playwright: false

**根拠**: pika は Tauri デスクトップアプリで、外部公開される **Web UI を持たない**（フロントは WebView2 内のローカルアプリシェル）。
UI 自動テストは初期版で持たず手動 acceptance で代替する（design doc 12章。将来 Playwright で系統C を部分自動化の余地はあるが本フェーズでは不採用）。
`sprints.json` にも `use_playwright: false` を明記する。

---

## 機能一覧（requirements.md の章に対応。本フェーズで Tauri 上に再建する範囲）

- **スパイク/結線・最薄プラットフォーム証明ループ（design doc 14章-1）**: cargo workspace＋Tauri scaffold → フォルダを開く→ツリー→タブ→CM6 編集→保存。この段で IPC コスト実測・データルート解決・テーマトークン・a11y 骨格を立証
- **アプリ起動・CLI・単一インスタンス・データルート（req 3章 / design doc 9章）**: `pika-cli.exe`（console subsystem・--help/--version/引数検証/終了コード）＋`pika.exe`（windows subsystem・GUI）の二段構成、自前 named pipe（`\\.\pipe\pika-<SID>`・ユーザー限定DACL・`PIPE_REJECT_REMOTE_CLIENTS`・受信≤数KB・JSONスキーマ検証）、`-g` パース、データルート解決（`portable.txt`）
- **メインウィンドウ・レイアウト・テーマ（req 11章 / design doc 5章・10章 / ui-design 2/7/9/12章）**: vanilla TS でモジュール分割した frontend（tree/tabs/status〔右下固定〕/notifications/editor〔CM6〕/diff）、ui-mock.html の CSS 変数トークンを単一源とするテーマ（ライト/ダーク/システム追従）、メニューバーは Tauri ネイティブメニュー
- **ファイルツリーと未読表示（req 4章 / design doc 4章・17章 / ui-design 5/6章）**: `role="tree"` ARIA・種別アイコン＋状態マーク（±/◆/取消線・伝播±淡）、自然順・除外（.git/node_modules）、シンボリックリンク循環検出、OneDrive プレースホルダ除外
- **エディタ・タブ・検索・保存（req 5章 / design doc 5章・8章・11章）**: CodeMirror 6 配線（外部リロード=単一トランザクション1Undo境界・非dirty）、タブの未読/未保存/削除済み重畳バッジ、エンコーディング往復＋保存中断フロー、fancy-regex 検索/置換、巨大ファイル段階制（CM6 実測後に閾値確定）
- **プレビュー（req 6章 / design doc 6章・7章 / ui-design 8/11章）**: 権限ゼロ別WebView＋custom protocol（`pika-preview://`）直配信、CSP レスポンスヘッダ強制、comrak(`unsafe_`)→ammonia サニタイズ、系統A（Markdown/差分/SVG・信頼JS）/系統B（HTML・JS無効）、Mermaid(`securityLevel:strict`)/KaTeX(`trust:false`)/highlight.js の危険オプション封じ、双方向スクロール同期（sourcepos）、プレビュー内検索
- **外部変更の反映と衝突処理（req 7章 / design doc 4章・11章）**: watcher 自前合成層（notify＋デバウンス/合体・rename正規化・自己保存抑制・オーバーフロー再同期・ポーリングフォールバック・F5）、`emit('fs-changed')`、クリーン時自動リロード、衝突退避・退避不能ガード
- **差分表示と既読（req 8章 / design doc 6章・7章 / ui-design 8/11章）**: similar 差分＋文字単位フォールバック、read-only unified レンダラ（行頭±記号・変更語下線/太字・F8/Shift+F8・色非依存）、3モード×差分トグル直交、確認済み/すべて確認済み（baseline-replace 退避）/巻き戻し（mtime/ハッシュ再照合）
- **スナップショット・状態保存（req 9章・10章 / design doc 11章）**: データルート配下 snapshots（windows crate で厳格DACL）、content-addressed object＋zstd、自己記述メタ、容量管理（10件LRU＋14日保護＋500MB＋90日GC・共有object全参照確認後削除）、state.json アトミック書込＋復元3分岐、最近使った項目＋ジャンプリスト（windows crate COM）
- **エッジケース・画像簡易ビュー（req 12章 / design doc 8章・19章）**: 非テキスト画像ビュー＋寸法プリチェック、非対応バイナリの「既定アプリで開く」、FSエッジ（シンボリックリンク循環・OneDrive・読み取り専用・ネットワークドライブ）、診断ログ（内容非記録・ローテーション）
- **アクセシビリティ・配布（req 11.5・13章 / design doc 10章・17章・18章）**: ARIA 全Web再構築（17章）、forced-colors/テキストスケール、WebView2 不在時フォールバック（18章）、Tauri bundler（ユーザー単位＋ポータブルzip・snapshots カスタムページ）、エクスプローラー統合（HKCU）、THIRD_PARTY_NOTICES
- **旧 canon 改訂（design doc 16章）**: requirements.md 1章/2.2/2.3/11.5・CLAUDE.md・design.md・ui-design.md・acceptance.md を Tauri へ整合（巨大ファイル数値は CM6 実測後に確定）

---

## 非対象（本フェーズで実装しないもの。情報落ち防止のため保全）

### 機能の再仕様化・UI 視覚仕様の作り直しはしない（design doc スコープ外）

機能は requirements.md が正、視覚仕様は ui-design.md/ui-mock.html が正で**流用**する。本フェーズは「Tauri 上でどう実現するか」のみ。
個別機能の詳細仕様変更を入れない（必要なら要件改訂を別タスクで提案）。

### 要件14章「やらないこと」（足さない。実装したくなったら要件改訂を提案）

IDE機能 / Gitクライアント / 内蔵AI・AIチャット / プロジェクト管理 / ビルド・実行・デバッグ / 本格コード編集 /
**任意JavaScript実行（ユーザー文書由来。同梱 Mermaid/KaTeX/ハイライトは別扱い）** / プラグイン機構 / WYSIWYG編集 /
フォルダ横断検索（grep）/ ホットエグジット / GUI設定画面（settings.toml 直編集で代替）/ 自動更新 /
**サイドバイサイド差分・レンダリング済みプレビュー上の差分** / 「次の未読ファイルへ」ジャンプ /
複数ウィンドウ・複数フォルダ同時オープン / CLI `--wait` / ショートカットのカスタマイズ / 永続Undo /
画像の編集・変換 / 日本語以外のUI言語 / ARM64ネイティブビルド / 外部 `.css` 読み込み / セクション単位既読・ハンク採否。

Tauri 化で UI フレームワークが Web になっても、これらの除外は維持する（ui-design 14章「見せ方の具体化であって機能追加ではない」と同じ立場）。

### 本フェーズの自動 verify に載せない（実機・実測が要る・系統C へ寄せる）

- design doc 15章 Open のうち実機・実測項目（IPC コスト・別WebView 権限ゼロ隔離・watcher オーバーフロー・CM6 巨大ファイル体感・性能・a11y・WebView2 不在時）
- 性能ゲート（起動0.5秒・プレビュー初回2.0秒・外部変更反映500ms・メモリ各上限・IPC ラウンドトリップ・18時間常駐リーク）
- WebView2 実描画・系統A/B JS 切替の順序保証・CM6 実編集・ライブリロード実動・画像簡易ビュー実描画・ナレーター読み上げ・Tauri bundler 実インストーラー・エクスプローラー統合・ジャンプリスト実機

---

## 補完した判断（元ドキュメントの不足・現状制約に対し、推測せず planner が明示）

design doc は「どう作るか（Tauri 載せ替え方）」までを定めるが、**スプリント分割・準備タスク・verify 手段の確定は本フェーズ planner の責務**
（design doc 20章「次の一手＝スプリント着手はユーザーが決める」）。後からのドリフト追跡用に根拠を残す。

1. **verify 二系統の根拠**。Tauri 化で `cargo test` が決定論ゲートの主役になる（pika-core は wry/WebView 非依存ゆえユニットテスト可能・design doc 4章/12章）。
   そこで (A) pika-core ロジックは `cargo test`＝**must**、(A補) command 層/TS は `cargo build`＋frontend 型チェックの**コンパイル/型成立**＝**must**、
   (C) 実機・実測は **手動 acceptance**（must verify に載せない）とする。design doc 1章「IPC コストも性能予算」「データを失わない最上位」と整合。

2. **現状は Tauri scaffold ゼロ・旧 C++ ツリー残存**（調査で確認）。実在するのは旧 `CMakeLists.txt`・`CMakePresets.json`・`vcpkg.json`・
   `vcpkg-configuration.json`・`src/`・`tests/`・`build/`・`installer/`(想定)・`docs/`・`.claude/`・`.github/` 等で、`Cargo.toml`・`src-tauri/`・`crates/` は未作成。
   Rust stable 1.96.0 は導入済み。退避ブランチ `wip/cpp-core-snapshot` は未作成。したがって **sprint 1 で「旧 C++ ツリーの退避＋除去＋Tauri scaffold」を行う**
   （design doc 13章のリポジトリ構成を立てる）。これは design doc 14章「最薄プラットフォーム証明ループ」の前提整備であり sprint 1 に同梱する。

3. **退避は履歴保全を最優先**（最上位原則「データを失わない」のリポジトリ版）。旧 `src/`・`tests/`・`CMakeLists.txt`・`CMakePresets.json`・
   `vcpkg.json`・`vcpkg-configuration.json`・`installer/`・`assets/`(wx 用)・旧 `.github/workflows` を退避ブランチ `wip/cpp-core-snapshot` へ**コミットしてから**本ツリーで除去する。
   `build/`・`vcpkg_installed/` は `.gitignore` 済みの生成物なので git からは除去不要だがディスク上は削除してよい。除去前に退避ブランチが push/保存できていることを sprint 1 の must とし、誤って履歴を失わない。

4. **巨大ファイル閾値（要件2.2/5.4/9.2）は CM6 実測まで TBD**。design doc 8章/15章-6 は「10MB 第1段階死守可否」を実測ゲートにし、確定値で要件を改訂する設計。
   よって **初期スプリントでは閾値を暫定（第1段階 10MB 維持・第2段階/上限は実測で確定）として実装し、実測を行う sprint（巨大ファイル担当）で確定値を埋め requirements.md 2.2/5.4/9.2 を改訂する**。
   初期の canon 改訂スプリントでは 2.2/5.4/9.2 を「TBD（実測 sprint で確定）」と明記し、推測値を canon に書き込まない。

5. **frontend 型チェックコマンドは scaffold 後に確定**。Vite+TS の `package.json` scripts（`tsc --noEmit` / `npm run build` 等）は sprint 1 で確定する。
   それまでの verify は `cargo test`/`cargo build` を主とし、scaffold 完了後のスプリントから frontend 型チェックを verify に追加する。
   `cargo audit` はネットワーク前提（advisory DB 取得）で安定実行できる段から must、それ以前は should に置く。

6. **編集直後 hook 制約**。`.claude/hooks/post-edit-check.mjs` は JSON 構文・C++ の clang-format を検査する。本フェーズの主言語は Rust/TS だが、
   生成する JSON（sprints.json・review-profile.json・tauri.conf.json・capabilities・package.json）は**厳密 JSON（コメント不可）**であること。
   Rust は `cargo fmt`、TS は frontend のフォーマッタで整形する前提（hook が C++ 専用なら Rust/TS は素通しだが、整形は各 verify／CI 側で担保する）。

7. **review-profile の調整**。Tauri 化で security の比重が最上位級に上がる（未信頼コンテンツ隔離・CSP・サニタイズ・named pipe 信頼境界＝設計の根幹リスク）。
   data（データを失わない・退避・state.json/index.json 復元）も最上位原則。frontend-ui（a11y 全Web再構築・状態重畳・ui-design 整合）は毎スプリント効く。
   product（要件整合・14章やらないこと）は機能追加スプリントで効く。changed_files に応じてルーティングする conditional 中心構成にしつつ、設計根幹リスクの security を厚めに倒す。詳細は `dev/review-profile.json`。

---

## スプリント設計（design doc 14章の実装順序を骨子に）

design doc 14章の縦切り順に並べる。中心体験（1〜中盤）を最優先で貫通させ、design doc 15章 Open 10項目を該当スプリントの must に、19章移植チェックリストを漏れなく割り付ける。

- **sprint 1 — 準備＋最薄プラットフォーム証明ループ＋canon 改訂着手**（design doc 14章-1・15章-1/3/5・16章。旧ツリー退避/除去・scaffold・IPC コスト実測・別WebView 権限ゼロ実証・WebView2 不在時）
- **sprint 2 — 監視（watcher 自前合成層）**（design doc 14章-2・15章-2・19章: rename継承/外部変更反映/FSエッジ）
- **sprint 3 — スナップショット/差分/確認済み（中心体験貫通）**（design doc 14章-3・19章: 衝突退避不能ガード/状態復元3分岐の一部）
- **sprint 4 — プレビュー（権限ゼロ別WebView）**（design doc 14章-4・6章・7章・15章-3 再確認・19章: 双方向スクロール同期/プレビュー内検索）
- **sprint 5 — CLI/単一インスタンス/状態復元**（design doc 14章-5・9章・15章-9・19章: 最近使った項目+ジャンプリスト/状態復元3分岐）
- **sprint 6 — 巨大ファイル/エンコーディング/検索置換（実測で 2.2/5.4/9.2 確定）**（design doc 14章-6・8章・15章-6/7/8・19章: CM6単一Undo/エンコーディング保存中断）
- **sprint 7 — a11y 仕上げ・エッジケース・配布**（design doc 14章-7・17章・18章・19章 残項目・docs/acceptance.md 整備）

各スプリント末に性能計測（Rust 側自己計測 QPC+psapi・design doc 3章/12章）を行い回帰を検知する（系統C）。

---

## 検証手段（まとめ）

### use_playwright: false（上記「検証戦略」参照）

### 自動検証（各スプリントの verify）

- **必須・決定論（系統A）**: `cargo test`（pika-core の diff/watcher/snapshot/encoding/render サニタイズ/search/settings/state/CLI 解析。exit 0 で合格）
- **必須・コンパイル/型（系統A補）**: `cargo build`（crates＋src-tauri・警告エラー扱い・exit 0）。scaffold 完了後は frontend 型チェック（確定コマンドを sprint 1 で確定）も追加
- **監査（系統A監査）**: `cargo audit`（CVE 追跡。安定実行できる段で must、それ以前は should）
- **整形（編集直後 hook＋CI）**: JSON 構文。Rust=`cargo fmt --check`／TS フォーマッタは CI 側。verify に重ねない

### 手動検証（系統C。docs/acceptance.md / docs/acceptance-findings.md 側）

- design doc 15章 Open の実機・実測項目（IPC コスト・別WebView 権限ゼロ隔離・watcher オーバーフロー・CM6 巨大ファイル・a11y・WebView2 不在時・プレビュー内検索）
- 性能ゲート（起動0.5秒・プレビュー初回2.0秒・外部変更反映500ms・メモリ各上限・IPC ラウンドトリップ・18時間常駐リーク）を各スプリント末に計測し劣化を即検知
- requirements.md 各章「受け入れ基準」を `docs/acceptance.md` のチェックリストへ写経し、各項目に対応する requirements/design doc 節を併記してリリース前に実施
