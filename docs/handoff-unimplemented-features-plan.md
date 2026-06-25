# 未実装機能の実装計画（検索/置換・画像簡易ビュー・設定反映・ネイティブダイアログ）

2026-06-25 作成・同日 **4観点サブエージェントレビュー反映でv2改訂**。`/clear` 後の「未実装でアクセスできない
ボタン」棚卸しを受けた実装計画。最後に **eval-loop（品質ループ）** を回す前提で、各ユニットを独立検証可能に切る。

実装は **dev-generator 委譲**、メインは誘導/検証/コミット判断に専念する（[[pika-impl-via-generator]] 方針）。

## レビュー反映（2026-06-25・4観点）

アーキテクチャ整合／セキュリティ／要件適合・スコープ／フロントエンド実装可能性 の4観点で計画を実コードに照合。
v2 で是正した主要点（詳細は各ユニットへ反映済み）:

- **U3 custom protocol は「メインWebView 向け登録」ではない**：`register_uri_scheme_protocol` はアプリ全体登録で
  権限ゼロ別WebView からも到達しうる。隔離の関門は **ハンドラ内の path ゲート**（origin 非依存）であると是正。
- **U3 寸法読取の供給元を明示**：`decide_image_open` は寸法を受けるだけ。`guard::image_dimensions` を pub 化し
  判定（`image_info`）と配信前ガードで同一実装を共有（判定の二重化で不整合を作らない）。
- **U3 封じ込めは `AccessControl::verify_read`＋4段防御**（verify_read→canonicalize 後に `is_sensitive` 再判定→
  暴走ガード→nosniff）を `local_resource_response` と同順で必須化。
- **U4 オフセット変換は単一パス O(L)**：per-match `encode_utf16().count()` は O(N·L) で巨大文書をブロックする。
- **U2a Tab は2系統**：表示幅 `EditorState.tabSize`（Facet）と Tab 挿入 `indentWithTab`＋`indentUnit.of("\t")` は別物。
- **U2b の `allow_remote_resources`／`sensitive_patterns` の設定駆動は安全側を弱めない不変条件付き**で配線。
- **U5 1件置換は backend に `replace_one` 新設**（`replace_in_text` は全文置換のみ・キャプチャ展開を Rust に閉じる）。
- **要件改訂2件**（6.1 初回既定モード＝Source 維持／第2段階の検索＝MVP 除外）を「## 要件改訂提案」に明記。

## 確定スコープ（ユーザー判断・2026-06-25）

- **U2 設定反映**：フロント可視分 ＋ **バックエンド消費分の配線も含む**（要件10.3 を充足。ただし下記不変条件付き）。
- **U4 検索**：**エディタ検索のみ（MVP）**。プレビュー内検索（要件5.4「プレビューのみ時 WebView2 find」）は
  権限ゼロ別WebView の IPC 不通のため **系統C 繰り越し**。**第2段階（50MB超）の検索も MVP 除外**（要件改訂提案）。
- **U3 画像**：**専用 custom protocol（AccessControl ゲート）**で配信（base64 data URL 案は不採用）。
- **初回既定モード**：**Source 維持**（要件6.1 を改訂提案）。
- **ネイティブダイアログ**：`tauri-plugin-dialog` を採用（`window.prompt` を置換）。

## 設計上の核心判断（リスクをテスト可能なRustへ寄せる）

1. **検索のオフセット変換（O(L) 単一パス）**：`pika-core::search` はバイトオフセット（UTF-8）を返す。CM6 は
   UTF-16 コードユニットで動くため、`src-tauri/document.rs` の command 層で **`char_indices()` を1回走査しながら
   バイト位置→累積 UTF-16 ユニット数を進め、各マッチ境界で記録**して UTF-16 オフセットを付与する
   （マッチは search_all が前方走査で昇順なのでソート不要）。**per-match `text[..b].encode_utf16().count()` は
   O(N·L) で禁止**。変換は cargo test で固定（ASCII／日本語 BMP／**絵文字サロゲートを複数マッチ混在**で網羅）。
   フロントは変換済み UTF-16 値をそのまま CM6 位置に使う（TS 側に未テストの変換を置かない）。
2. **画像の判定と配信の分離（寸法読取は単一実装を共有）**：判定（上限超→外部誘導／非対応バイナリ／表示可）は
   `image_info` command に集約。寸法は **pub 化した `render::guard::image_dimensions`** で読み `nontext::decide_image_open`
   へ渡す。バイト配信は **専用 custom protocol** が担い、配信前ガードも同じ `image_dimensions`/`check_image_bytes`
   を使う（`image_info`=表示可なのに protocol が 413、の不整合を作らない）。

---

## U1. ネイティブ選択ダイアログ（最小・基盤）

**目的**：要件3.2/11.2。「フォルダを開く（Ctrl+Shift+O）」「ファイルを開く（Ctrl+O）」の `window.prompt`
パス手入力を OS ネイティブ選択ダイアログへ置換する。

**バックエンド**
- `src-tauri/Cargo.toml`：`tauri-plugin-dialog = "2"` 追加（公式・軽量）。
- `src-tauri/src/main.rs`：`.plugin(tauri_plugin_dialog::init())` を登録。
- `src-tauri/capabilities/main.json`：`"dialog:allow-open"` を追加（**最小権限**＝保存ダイアログ `allow-save` は付けない。
  「名前を付けて保存」は本スコープ外だが、将来要る点を下記トレーサビリティに残す）。

**フロント**
- `package.json`：`@tauri-apps/plugin-dialog` 追加。
- `src/main.ts`：`onOpenFolder` → `open({ directory: true, defaultPath: state.folder ?? undefined })`、
  `onOpenFile` → `open({ multiple: false, directory: false })`。キャンセル（null）は従来どおり何もしない。
  **返却パスは必ず既存の `switchFolder`/`openPath` 経由**で `open_workspace`/`read_file` に渡す
  （＝`AccessControl`（`set_root`/`verify_read`）で core 再検証される。ダイアログ結果を直接 FS read する近道
  command は新設しない＝迂回経路を作らない）。
- devブラウザ単体（Tauri 非依存）では `open` 不在のため存在チェックで従来 prompt にフォールバック（実機外でも壊さない）。

**検証**：`cargo build`／`npm run typecheck`／実機でフォルダ・ファイル選択がダイアログで開ける。

---

## U2a. 設定反映（フロント可視分）

**目的**：要件10.3/10.4。起動時 `get_settings` と `onSettingsChanged` で、ユーザーが見て効く設定を即適用する。

対象（フロントで適用）：
- **`theme`（light/dark/system）**：初期化順序を **`initTheme()` → `getSettings()` → `applyTheme(settings.theme)`** に
  確定（`applyTheme` 引数 `ThemeMode` は `ThemeSetting` と同型で変換不要）。`onSettingsChanged` にも `applyTheme` を
  足す。settings 取得前の暫定テーマ→取得後の確定適用を **1フレームに抑えて FOUC（ちらつき）を回避**する。
- **`wrap_default`**：`state.lineWrapping` の初期値。以後の `createEditor` 初期値＋既存 `wrapCompartment` で反映。
- **`tab_width`（2系統を分けて結線）**：
  - (a) **表示幅** ＝ `tabSizeCompartment.of(EditorState.tabSize.of(tab_width))`（Facet を `.of` でラップ。
    既存 `wrapCompartment` と同型。設定変更時は `reconfigure(EditorState.tabSize.of(n))` で内容/カーソルを壊さず反映）。
  - (b) **Tab 挿入** ＝ `keymap.of([indentWithTab])`（`@codemirror/commands`）＋ `indentUnit.of("\t")`
    （`@codemirror/language`）。要件5.2「Tab はタブ文字を挿入（スペース展開しない）・保守的既定」を満たす。
    `insertTab` は「選択時 `indentMore`／カーソル時タブ文字挿入」で、`indentUnit` をタブにすることで選択インデントも
    タブになる。**`tab_width` は (a) の表示幅のみに使う値**（挿入スペース数ではない）。
- **`default_mode`（source/preview）**：ファイルを開いた直後の初期 `viewMode`。**既定は Source を維持**（要件6.1 改訂提案。
  下記「## 要件改訂提案」参照）。**ファイル種別ごとのモード記憶（要件6.1）は state.json 拡張要のため MVP 外**
  （トレーサビリティで未充足を明示）。

**触るファイル**：`src/main.ts`（`applySettings(s)` を新設し起動時・`onSettingsChanged` 両方から呼ぶ）、
`src/editor/index.ts`（`createEditor` に `tabWidth` 引数＋`tabSizeCompartment`＋`indentWithTab`/`indentUnit` 結線）、
`src/theme/index.ts`（settings 起点の初期化順序）。

**検証**：`npm run typecheck`／実機で settings.toml の theme/wrap_default/tab_width/default_mode 編集保存→
再起動なし反映（要件10.4）。Tab キーでタブ文字が入る（スペース展開しない）。

---

## U2b. 設定反映（バックエンド消費分の配線）

> **【2026-06-25 スコープ確定・高価値サブセット】** ユーザー判断で U2b は**高価値3項目に限定**して実装した
> （実装済み・commit 群）:
> - **excluded_dirs**（ツリー列挙の除外・commit a4c00b1）
> - **sensitive_patterns**（is_sensitive_with／baseline_policy_with の和集合・既定を外せない・配信拒否＋平文回避・commit 9fc70ab）
> - **feature_mermaid/math/highlight**（wrap_preview_document_with の機能ゲート・commit e4c6d42）
>
> **系統C／将来送り（MVP 除外・実装しない）**＝上級者向け/エッジで費用対効果が低く、軽量さ＞開発効率の方針に従い後回し:
> - `huge_file_threshold_bytes` / `long_line_chars`（pika-core 純粋関数の引数化＋下限クランプ）
> - `allow_remote_resources`（プレビュー外部リソース既定・CSP／build_csp 変更を伴う）
> - `full_hash_on_startup`（復元時ハッシュ照合の有無）
>
> 下記の元計画はこの確定により部分実装（上記3項目のみ）。残り3項目の節は将来の参考として残す。

**目的**：要件10.3 の残り。`SettingsService` が `get_settings` でしか使われておらず、バックエンド挙動は
pika-core のハードコード既定で動いている。設定値を各 command／pika-core 関数へ通す。**ただし安全側・中心体験を
設定で弱めない不変条件を守る**。

**配線対象（command 層で `State<Arc<SettingsService>>` を取り、`snapshot()` の値を pika-core へ引数で渡す）**
- `excluded_dirs` → `commands.rs` の `open_workspace`/`list_dir`（ツリー列挙の除外）。
- `huge_file_threshold_bytes` / `long_line_chars` → `document.rs` の `open_document`（段階判定の閾値）。
  **【不変条件】中心体験死守ライン（実測確定の第1段階＝10MB 等・要件2.2/5.4/9.2）を設定で下回らせない**。
  設定値は **下限ガードでクランプ**する（例：閾値は確定下限以上のみ採用。下限未満はクランプ＋警告）。
- `sensitive_patterns` → 機密判定。**【不変条件】既定パターン（`.env`/`.key`/`.pem`/`secret`）は設定で外せない＝
  「設定は機密パターンを足せるが減らせない（和集合）」**。`pika-core::snapshot::policy` に
  `is_sensitive_with(path, extra_patterns)` を新設し、既定判定を常に内包。preview.rs のローカル配信機密拒否・
  path.rs・U3 protocol も同じ関数を使う。
- `allow_remote_resources` → プレビュー外部リソース既定許可。**【不変条件】`*`（無制限）を CSP に入れない・`http://` は
  遮断維持・「文書を開いただけで外部通信が起きない」（要件2.4）を破らない**。`true` の意味は **「外部参照を検知した
  https ホストを都度確認なしで自動許可（許可候補に積む）」**に限定する（`build_csp` のワイルドカード拒否と整合）。
  フロントの「タブ切替で `allowExternal` を既定オフに戻す」（main.ts）と整合させ、既定オン時は検知ホストを自動 allow
  に積む形にする。
- `feature_mermaid`/`feature_math`/`feature_highlight` → プレビュー信頼 JS の**条件注入**。
  **【影響大】`pika-core::render::wrap_preview_document` に機能フラグ引数を追加＝公開 API のシグネチャ変更**。
  src-tauri の全呼び出し点（`document_response` 等）＋ pika-core/preview.rs の多数の cargo test を同時改修する
  （見積りに計上）。
- `full_hash_on_startup` → 起動時の未読照合（restore 経路のハッシュ照合の有無）。

**回帰ゼロの担保**：pika-core の純粋関数を**引数化**し（pika-core に serde/Tauri 型を持ち込まない＝プリミティブで渡す）、
**引数のデフォルトを現定数と一致**させて既存 cargo test（pika-core 396／pika-app 34）の回帰をゼロにする。
引数化した各関数に「設定値で挙動が変わる」テストと「不変条件（機密は減らせない・閾値は下限クランプ）」テストを追加。

**反映タイミング**：MVP は「次に開く操作から反映」で可（要件10.4「再起動なしで反映」を満たす）。プレビュー機能
フラグは再プレビューで即反映。

**検証**：`cargo test`（引数化後の回帰ゼロ＋新規テスト緑）／`cargo build`／実機で excluded_dirs・feature_* 等が効く。

---

## U3. 画像簡易ビュー（custom protocol）

**目的**：要件12.2。画像（png/jpg/gif/webp/bmp/ico）を簡易ビューで表示（fit/actual 切替）。上限超は外部誘導、
非対応バイナリは「対応していない形式」＋外部誘導。現状 `renderImageView`/`renderOpenExternally` はデッドコードで、
画像を開くとトーストのみ＆テキストとして CM6 に読み込まれる。

**バックエンド**
- **`render::guard::image_dimensions` を pub 化**（現状 private・guard.rs）。`image_info` と protocol が同一実装で寸法を読む。
- **判定 command `image_info(path)`**：`AccessControl::verify_read(path)` で封じ込め検証 → `image_dimensions` で寸法 →
  `nontext::decide_image_open` ＋ `classify_extension` で `{ kind: "image", width, height, mime }` /
  `{ kind: "too-large" }` / `{ kind: "unsupported" }` を返す。判定ロジックは cargo test 済み＋配線テスト追加。
- **専用 custom protocol（`pika-asset://`、実体 `http://pika-asset.localhost/`）**を `main.rs` の
  `.register_uri_scheme_protocol` で**アプリ全体に登録**（WebView 限定ではない）。**隔離の関門は発信元に依らず
  ハンドラ内の path ゲートのみ**。ハンドラは `local_resource_response`（preview.rs:468-510）と**同順の4段防御**:
  1. 受けたパスを `AccessControl::verify_read` に通す（絶対パス検証＋canonicalize＋root/allowed 配下。**自前のパス
     正規化/許可判定は書かない**）。
  2. 解決後の実体名で `snapshot::policy::is_sensitive_with` を再判定（機密拒否＝シンボリックリンク偽装も塞ぐ）。
  3. 暴走ガード `check_image_bytes`（画像）／`check_svg_bytes`（SVG が `<img src>` 経由で来る場合）で上限超は 413。
  4. `Content-Type`（既存 `guess_content_type` 流用）＋`X-Content-Type-Options: nosniff`＋控えめキャッシュで配信。
- **CSP（`tauri.conf.json`）**：メインWebView の `img-src` に `http://pika-asset.localhost` を **1ソースのみ追加**。
  `default-src`/`script-src`/`connect-src` は触らない。base64 案を捨てたので `data:` は不要なら削る検討。
  **プレビュー別WebView の CSP（レスポンスヘッダ・別系統）には `pika-asset.localhost` を足さない**（未信頼文書から
  画像 protocol を引く面を CSP で塞ぐ）。

**フロント**
- `index.html`：`#editor-host`/`#diff-host`/`#preview-host` と並ぶ **`#image-host`** を `#editor-pane` 配下に追加。
- `src/ui/image.ts`：既存 `renderImageView`/`renderOpenExternally` を流用。fit/actual トグル UI を画像ビュー上端に置く。
- `src/main.ts`：
  - **`OpenTab` に `isImage` フラグを導入**。`openFile` で `classify_extension` 相当（`isImageExt`／非対応バイナリ）を
    判定し、画像/非対応バイナリは **CM6 を作らず** `image_info` で分岐
    （image→`renderImageView`（protocol URL）／too-large・unsupported→`renderOpenExternally`（`onOpenExternal`））。
  - **画像タブのアクティブ化では `state.editor?.destroy(); state.editor = null`** にして、`refreshStatus`
    （`!state.editor` 早期 return）・`captureActivePosition`・`autoReloadCleanTabs` 等の editor 依存箇所が null ガードで
    素通る状態にする（前タブのメトリクス誤表示を防ぐ）。
  - **`applyOccupancy` に画像分岐を追加**：`imageHost().hidden = !showImage`、画像占有時は editor/diff/preview を全部隠す。
    画像表示時は **`hidePreview` を必ず呼ぶ**（プレビュー子WebView は OS レベルの子ウィンドウで z-index 制御不可＝
    `#image-host` の上に残る）。占有世代（PreparedPreview.generation）の直列化に画像モードを正式に組み込む。
    `refreshStatus` は画像占有時 early-return（カーソル/行/文字なし）。
  - 画像も変更検知・未読バッジ対象（要件12.2）：ベースラインはハッシュのみ（差分/巻き戻し非対象）。既存 unread 機構に乗る。

**検証**：`cargo test`（image_info 判定＋protocol の4段防御の配線テスト）／`cargo build`／`npm run typecheck`／
実機で通常画像表示・fit/actual・巨大画像で外部誘導・非対応バイナリで外部誘導・画像の未読バッジ。

---

## U4. 検索バー（最大・難所）

**目的**：要件5.4。ファイル内検索（Ctrl+F）：インクリメンタル・大文字小文字区別・単語単位・正規表現・
ヒット件数表示・全ヒットハイライト・前後移動。**ソース/分割/差分ON のエディタ（テキスト）検索**のみ（MVP）。

**バックエンド（小改修）**
- `src-tauri/document.rs` の `search_in_text`：戻りの各マッチに **UTF-16 オフセット**（`utf16_start`/`utf16_end`）を
  **追加**（byte は残す）。**`char_indices()` の単一パス累積変換 O(L)** で算出（per-match `encode_utf16().count()` 禁止）。
  cargo test に **複数マッチ×サロゲートペア混在**で全マッチの UTF-16 オフセットが正しいケースを追加。
- `pika-core::search` 自体は不変（バイトオフセットのまま）。変換は src-tauri 境界で行う。

**フロント（新規 `src/search/` 配下。`@codemirror/search` は使わず自前 StateField+Decoration）**
- **検索バー UI（全Web描画）**：`#editor-pane` 上端に重ねるバー。入力欄・件数表示（`3/27` 形式）・前へ/次へ・
  case/word/regex トグル・閉じる（Esc）。`Ctrl+F` で開きフォーカス、`Esc`/閉じるで CM6 へ戻す。ui-design トークン準拠。
  **現状 `dispatchAction("find")` のトースト stub を実バー起動へ置換**。
- **キー処理**：検索バー入力欄では keydown を **`stopPropagation`** して window の `resolveShortcut` ディスパッチへ漏らさない
  （Ctrl+F/Esc/Enter/Shift+Enter をバー内で完結。二重発火防止）。`Enter`=次、`Shift+Enter`=前。
  （注：`@codemirror/search` 未使用＝CM6 既定検索は存在しないので「既定検索を奪う」競合は無い。）
- **検索状態機械**：入力 debounce（~120ms）→ `searchInText(doc, query, options)` を invoke。新しい検索が前回をキャンセル
  （backend `SearchCancelService` 済み）。結果（UTF-16 オフセット群）を保持。**巨大文書（第1段階＝最大数十MB）では
  打鍵ごとに全文を invoke 転送する**ため、debounce 延長 or 抑制を検討（IPC 予算・原則②）。全文転送コストを後回しリスクとして可視化。
- **CM6 全ヒットハイライト**：`StateField`＋`Decoration.mark`（クラス `cm-search-hit`／現在ヒット `cm-search-hit-current`）。
  色は app.css トークンでテーマ追従。現在ヒットへ `view.dispatch({ selection, effects: scrollIntoView })`。
- **モード連動（要件5.4）**：ソース/分割/差分ON は本エディタ検索。プレビューのみ（差分OFF）では検索バーを出さず
  Ctrl+F は no-op（プレビュー内検索＝WebView2 find は系統C 繰り越し）。
- **段階制**：**第2段階（読み取り専用・`editingOff`）は検索バー無効化**（要件改訂提案。下記参照）。通常/第1段階に限定。

**検証**：`cargo test`（UTF-16 単一パス変換・サロゲート混在）／`npm run typecheck`／実機で日本語・正規表現・件数・
全ハイライト・前後移動・Esc 復帰。

---

## U5. 置換バー

**目的**：要件5.4/5.6。置換（Ctrl+H）：1件ずつ／全置換、正規表現置換（キャプチャ参照）。
**ソース・分割でのみ**（差分は読み取り専用＝置換不可）。

**バックエンド（改修）**
- **`replace_one` command を新設**（`document.rs`）：`replace_one(text, query, replacement, options, from_byte)
  -> { text, replaced_range, next_match }` 相当。**キャプチャ参照展開（`$1` 等）を Rust 側（cargo test 済み経路）に閉じる**
  （`replace_in_text`/`replace_all` は全文置換のみで単一ヒット置換の口が無いため。フロントでキャプチャ展開を実装しない）。
  pika-core::search に単一ヒット置換ヘルパを追加し cargo test。

**フロント（U4 を拡張）**
- 検索バーに置換行（置換語入力・置換/すべて置換ボタン）を足す（`Ctrl+H` で置換行付きで開く）。
- **すべて置換**：`replaceInText(doc, query, replacement, options)` → 返却 `text` を CM6 へ単一トランザクションで反映
  （`setContent` 相当・**dirty 化**＝外部リロード注釈は付けない・カーソル維持）。件数を通知。
- **1件ずつ**：現在ヒット位置から `replace_one` を呼び、返却で CM6 を更新して次ヒットへ。
- **活性制御**：差分ON／プレビューのみ／第2段階（読み取り専用）では置換を無効化（要件5.4）。

**検証**：実機で「正規表現＋キャプチャ参照で全置換ができる」（要件5.6）。`npm run typecheck`／`cargo test`（replace_one）。

---

## 推奨実装順序と依存

1. **U1 ダイアログ**（独立・最小・de-risk）
2. **U2a 設定フロント可視**（小・editor 改修が U4/U5 の土台にも効く）
3. **U3 画像**（中・backend protocol＋占有拡張）
4. **U4 検索バー**（大・難所）
5. **U5 置換バー**（U4 拡張）
6. **U2b 設定バックエンド配線**（大・横断・公開API破壊を伴う。視認機能を出し切ってから完全性を詰める）
7. **最終 eval-loop**（全体横断の品質ループ）

U2b は U3/U4 と独立に進行可。U5 は U4 完了が前提。

---

## 要件改訂提案（CLAUDE.md「要件に無ければ要件改訂を提案」に基づく）

実装/MVP 判断と要件本文が食い違う2点。ユーザー判断（2026-06-25）で下記方針に確定し **2026-06-25 改訂済み**
（requirements.md 6.1・2.2 第2段階・5.4・2.1 性能表／ui-design.md §モード／tauri-rewrite design spec §8／
acceptance.md TF6・acceptance-findings.md L6 を整合修正）。

1. **要件6.1 初回既定モード（Source 維持）**
   - 現要件：「初回の既定はプレビューのみ」。実装（pika-core settings.rs `default_mode: Source`）は Source 既定で逆。
   - 改訂案：「**初回既定モードは設定（10.3）で選択でき、既定値は Source（軽量起動重視）**」。看板60MB〔2.1〕は
     WebView2 未起動のソースモード参考値と整合。`default_mode` 設定で Preview に変更可能。
2. **要件2.2/5.4 第2段階の検索（MVP 除外）**
   - 現要件：2.2「第2段階＝閲覧・検索のみ」／5.4「第2段階で無効化するのは置換のみ（検索は維持）」。
   - 実態：第2段階は CM6 へ全量ロードしていない（`text` 空）ため、エディタ検索がそのままは効かず backend 経由
     ストリーミング検索が必要。
   - 改訂案：「**第2段階（50MB超・読み取り専用）は閲覧のみ。検索・置換は通常・第1段階（〜50MB）に限定し、
     第2段階の検索は系統C 繰り越し（MVP 外）**」。U4 は `editingOff` で検索バーを無効化。

---

## 最終 eval-loop（品質ループ）の設計

メモリの前例（jq 必須・閾値80・4ゲート独立再実行・PrintWindow 実機確認）に倣う。

**合否ゲート（系統A＋系統C）**
1. `cargo test`（pika-core ＋ pika-app・exit 0。新規テスト緑＋既存回帰ゼロ）
2. `cargo build`（crates＋src-tauri・**warnings=deny**）
3. `npm run typecheck`（`tsc -p tsconfig.app.json` strict）
4. **PrintWindow 実機スクショ**で機能別手動シナリオを観測（GUI 起動＝Vite 先行＋`cmd //c start` デタッチ）

**機能別 手動シナリオ（系統C）**
- U1：Ctrl+O / Ctrl+Shift+O でネイティブダイアログが開き、選択でファイル/フォルダが開く。
- U2a：settings.toml の theme/wrap_default/tab_width/default_mode 編集→再起動なし反映。Tab がタブ文字挿入。
- U2b：excluded_dirs がツリーに効く／feature_* でプレビュー注入が出し分く／allow_remote_resources の自動許可
  （`*`不可・http遮断維持）／sensitive_patterns は既定を外せない（和集合）／巨大閾値は下限クランプ。
- U3：通常画像表示・fit/actual・巨大画像で外部誘導・非対応バイナリで外部誘導・画像の未読バッジ・
  protocol が機密/脱出/上限超を拒否。
- U4：Ctrl+F で日本語/正規表現検索・件数・全ハイライト・前後移動・Esc 復帰。プレビューのみ/第2段階で無効。
- U5：Ctrl+H で1件/全置換・正規表現キャプチャ参照置換・差分ON/プレビュー/第2段階で無効。

**後回しの明示（silent cap 回避）**
- プレビュー内検索（要件5.4 WebView2 find）＝系統C 繰り越し。
- 第2段階（読み取り専用・巨大）の検索＝MVP 除外（要件改訂提案。通常/第1段階に限定）。
- 表示モードのファイル種別別記憶（要件6.1）＝state.json 拡張要・MVP は `default_mode` 初期値まで。
- **U2b の `huge_file_threshold_bytes`/`long_line_chars`・`allow_remote_resources`・`full_hash_on_startup`
  ＝MVP 除外（2026-06-25 高価値サブセット確定）**。設定 UI/取得（U2a 機構）には現れるが backend 挙動は
  既定固定（pika-core ハードコード）。系統C/将来送り。

## 要件トレーサビリティ

- U1 → 3.2 / 11.2
- U2a → 10.3 / 10.4（theme/wrap_default/tab_width/default_mode＋ 5.2 Tab挙動）／**6.1 は初回既定のみ充足（種別別記憶＝MVP外・要件改訂提案）**
- U2b → 10.3（excluded_dirs）／9.1（sensitive_patterns＝配信拒否＋平文回避・和集合で既定を外せない）／6.2（feature_*＝プレビュー機能トグル）。**閾値/remote/full_hash は MVP 除外（2026-06-25 確定・上記後回し参照）**
- U3 → 12.2（＋ 2.2 暴走ガード・9.1 機密配信拒否）
- U4 → 5.4（検索。第2段階検索＝MVP外・要件改訂提案）
- U5 → 5.4 / 5.6（置換・キャプチャ参照全置換）
- 将来：`dialog:allow-save`（名前を付けて保存・本スコープ外）

## 関連メモリ

[[pika-editor-project-status]]（プロジェクト状況）／[[pika-impl-via-generator]]（generator委譲）／
[[review-findings-fix-complete]]（検証ループ・generator虚偽報告対策）
