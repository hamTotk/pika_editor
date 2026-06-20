# pika Tauri フル刷新 アーキテクチャ設計

- 作成日: 2026-06-20
- 版: **v2（3観点サブエージェントレビュー反映）**。v1 からの主な変更: 未信頼プレビューを sandboxed iframe → **別WebView＋権限ゼロ**に変更（Windows の iframe invoke 例外 CVE-2024-35222 回避）／CLI を AttachConsole 単一バイナリ → **二段構成（console stub + GUI）**に変更／プレビューHTMLは invoke でなく **custom protocol 直配信**（IPCコスト・オリジン分離）／comrak は `unsafe_=true`→ammonia 順を確定／正規表現は **fancy-regex 第一候補**／**アクセシビリティの全Web再構築**（17章）・**WebView2不在時フォールバック**（18章）・**移植チェックリスト**（19章）を新設。
- 位置づけ: pika を wxWidgets/C++ 実装から **Tauri（Rust + WebView2 + HTML/CSS/TS）へフル刷新**する際のアーキテクチャ設計。機能要件は `docs/requirements.md`（全14章）を**そのまま正典として引き継ぐ**。本書が決めるのは「Tauri スタックへの載せ替え方」。
- 上書き対象: `requirements.md` 1章（旧技術スタック）・2.2（巨大ファイル閾値）・2.3（WebView2不在時）・11.5（a11y 実装手段）、`CLAUDE.md`／`docs/design.md` の「ネイティブ優先・WebView2 はプレビュー/差分のみ・UI系依存追加禁止」原則、`docs/ui-design.md` の wx 直書き箇所。差し替え計画は16章。
- スコープ外: 機能の再仕様化（requirements.md が正）、UI 視覚仕様の作り直し（ui-design.md/ui-mock.html が正・流用）。

---

## 1. 目的・制約・成功条件

### 目的
VSCode/Obsidian 同等の「全Web描画＋CSS変数トークン」由来の質感を、wx 版（9.6MB）と同等以下の軽さで両立する。Tauri は OS の WebView2 を使い Chromium を同梱しない。スパイク実測で **8.6MB**（wx 版 9.6MB より小）を確認済み。pika は元々プレビューで WebView2 を使っており地続き。

### 制約（設計原則は `design.md` 1章を継承）
1. **データを失わない** — 退避スナップショットが最後の砦。index 破損時も object の自己記述メタから復元。
2. **固まらない** — UI（WebView）スレッドで 200ms 超ブロックしない。重い処理は Rust 側スレッド。**IPC ラウンドトリップ自体もコスト**として性能予算に計上する（3章）。
3. **軽い** — 未使用機能のコストはゼロ。依存追加はバイナリサイズと天秤。
4. **足さない** — requirements.md 14章に無い機能は作らない。
5. **速く作る** — 上記を満たす範囲で最も単純な実装。

新補助原則（旧「ネイティブ優先」を置換、16章で正式化）:
- **コアは UI を知らない** — `pika-core`（Rust・Tauri/wry 非依存）は UI も WebView も知らない。橋渡しは Tauri command/event のみ。
- **未信頼コンテンツはアプリシェルから物理隔離する** — 文書由来の HTML/Markdown は**権限ゼロの別 WebView**で描画し、Tauri API から物理分離（6章）。
- **ワークスペースを汚さない** — ユーザーフォルダに pika のファイルを一切作らない。

### 成功条件
- 中心体験（開く→外部変更反映→差分→確認済み）が Tauri 上で貫通。
- 性能目標（2.1）を満たす（巨大ファイル閾値のみ8章でWeb現実値へ改訂）。**IPCコストを性能予算に織り込んでなお目標内**。
- 配布サイズ 30MB 以内。
- コアロジックが `cargo test` で自動検証（旧 gtest 735 件相当の決定論ゲート再建）。
- **アクセシビリティ（要件11.5 を全Web向けに改訂した水準）を満たす**（17章）。

---

## 2. 新技術スタック（requirements.md 1章を上書き）

| 項目 | 決定 | 理由（要点） |
|---|---|---|
| アプリフレームワーク | Tauri 2 | OS WebView2 利用で Chromium 非同梱＝極小バイナリ。Rust backend。 |
| backend 言語 | Rust（stable 1.96+） | ネイティブ・単一ランタイム・FFIグルー不要。 |
| UI 描画 | HTML/CSS/TypeScript（全Web） | OSコントロール非依存＋CSS変数トークンで質感。 |
| エディタ部品 | CodeMirror 6 | Obsidian 採用。軽量・モジュラー・Markdown 得意・MIT。 |
| **プレビュー/差分描画** | **権限ゼロの別 WebView/ウィンドウ**（custom protocol 直配信＋CSP） | 未信頼文書をアプリシェル（Tauri API）から物理隔離。Windows の同一オリジン iframe invoke 例外（CVE-2024-35222）を回避＝旧「別プロセス WebView2」隔離に最も近い。 |
| Markdown 変換 | comrak（GFM・**`unsafe_=true`** で raw HTML 透過） | Rust・高速・GFM。raw HTML を通し**最終 ammonia でサニタイズ**（要件6.2「Markdown内HTML対応」のため。7章）。 |
| HTML サニタイズ | ammonia（ホワイトリスト方式・最終段固定） | `<script>`・イベント属性・`javascript:`・`<iframe>/<object>/<embed>/<base>/<meta>` 等除去（要件6.2）。 |
| 差分エンジン | similar（Myers）＋**文字単位フォールバック自前**（unicode-segmentation） | 旧 dtl 相当。日本語等の語境界不成立行は grapheme 単位へ（要件8.2。7章）。 |
| 検索/置換の正規表現 | **fancy-regex（第一候補・純Rust）／pcre2（機能不足時のみ）** | 要件5.4 の後方参照・キャプチャ参照・Unicode文字クラス。fancy-regex は C 依存なしで cargo 単一ツールチェーンの動機に整合。採否は機能テスト通過で判定（要件5.4「具体エンジンは設計で確定」）。ReDoS 対策にマッチタイムアウト必須。 |
| ファイル監視 | notify crate（raw event 供給）＋**自前合成層**／オーバーフロー検知不可なら windows crate で `ReadDirectoryChangesW` 直叩き | デバウンス/合体・rename正規化・自己保存抑制・オーバーフロー(ERROR_NOTIFY_ENUM_DIR)再同期・ポーリングは自前（要件7.1。4章）。notify のオーバーフロー表面化は要検証（15章）。 |
| 圧縮 | zstd crate | スナップショット内容圧縮。 |
| ハッシュ | twox-hash（XXH3） | LF正規化後の内容ハッシュ・重複排除。 |
| 設定パース | toml crate | settings.toml 読取専用（pika は書き戻さない）。 |
| エンコーディング | encoding_rs | BOM/UTF-8/UTF-16/Shift_JIS 判定・往復。**表現不能文字検出は encoder の unmappable を自前ハンドリング**（要件5.2。11章）。 |
| 単一インスタンス | **自前 named pipe を既定実装**（`\\.\pipe\pika-<SID>`・ユーザー限定DACL・`PIPE_REJECT_REMOTE_CLIENTS`・受信≤数KB・JSONスキーマ検証・受理=パスオープン限定） | 要件3.2 の信頼境界。tauri-plugin-single-instance は全項目を満たす証跡が取れた場合のみ利便部分を流用（9章）。 |
| CLI | **二段構成**: `pika-cli.exe`（console subsystem・--help/--version/引数検証/終了コード）＋ `pika.exe`（windows subsystem・GUI） | GUI subsystem 単体では `AttachConsole` でもシェル制御戻りを保証できない（要件3.4）。旧 `pika.com` 二段構成を Rust で再建。 |
| エラー型 | thiserror による `Result<T, PikaError>` | コア公開API は Result 方式。 |
| ビルド/バンドル | cargo（workspace）＋Vite＋Tauri bundler | フロントは TypeScript。 |
| 依存監査 | `cargo audit` を CI ゲート | comrak/ammonia/Mermaid/KaTeX 等の CVE 追跡（12章）。 |

WebView2 Runtime 前提は requirements.md 2.3 をベースにするが、**全Web化により「不在時もエディタ/ツリーは動く」は構造的に不可**＝要件2.3 を改訂（18章・16章）。

---

## 3. アーキテクチャ骨格

レイヤー依存は一方向（UI 層の実体だけ Web に置換）:

```
TS frontend (UI 層: tree/tabs/editor/status/notifications)          [メインWebView]
        │  invoke(command)        ▲  emit(event)                     │
        ▼                         │                                  │
Tauri command/event 境界 (アプリ層: src-tauri/commands, ipc, protocol, cli)
        │  Rust 関数呼び出し                                          │
        ▼                                                            ▼
pika-core クレート (コアサービス層)                       custom protocol が直配信
 document/workspace/watcher/diff/snapshot/render/search/settings    ▼
        │                                            [プレビュー別WebView: 権限ゼロ]
        ▼                                            未信頼HTML（Tauri API 到達不能）
プラットフォーム層 (Win32 / WebView2 / FS)
```

- frontend → backend は `invoke('cmd', args)`。backend → frontend は `emit('event', payload)`（ワーカースレッドから）。
- **逆参照禁止**: `pika-core` は Tauri も WebView も知らない。command ハンドラが薄い境界。
- 重い処理（監視・スナップショット・差分・レンダリング・巨大ファイル読取）は **Rust 側スレッド**で行い、UI スレッドを 200ms 超ブロックしない。

### IPC コスト予算（性能要件の根幹・v2 で新設）
Tauri 既定 IPC は JSON 文字列化を伴い、ペイロードが大きいとシリアライズコストが性能目標（プレビュー更新300ms・保存・起動0.5秒）を直撃する（64KB blob のラウンドトリップ実測 ≈6.7ms、JSON invoke は不利）。そこで**転送方式をデータ種別で使い分ける**:

| データ | 転送方式 | 理由 |
|---|---|---|
| プレビュー HTML（サニタイズ済み） | **custom protocol が別WebViewへ直配信**（invoke で返さない） | シリアライズ回避＋メインワールドを経由させずオリジン分離を保つ（6章）。フロントは別WebView の URL を切り替えるのみ。 |
| 保存（編集バッファ全量） | Channel API（バイナリ/ストリーミング） | 10MB 級を JSON 文字列化しない。 |
| 巨大ファイルの range 読取 | custom protocol の range 配信 or Channel | 8章。 |
| 差分 hunk 配列 | invoke（中規模）／大きい場合は Channel | 変更が多いと巨大化しうる。 |
| ツリー・状態・小データ | invoke（JSON） | 小さく頻度低。 |

各段末の性能計測でこの予算に対する回帰を検知する（旧 design.md §11 の予算分解を Rust 側自己計測 QPC+psapi で再建）。

### データフロー例
- 開く: `invoke('open_workspace',{path})` → core が初回ベースライン取得をバックグラウンド起動しつつツリー＋復元状態を返す。
- 外部変更: watcher スレッド → `emit('fs-changed',{paths})` → フロントが未読バッジ更新。
- 編集→保存: 編集は CM6 のフロント状態に閉じる。保存は Channel 経由で content を渡し、core が encoding_rs で書込（**表現不能文字は保存前検査で中断**＝11章）・自己保存ハッシュ抑制を登録。
- プレビュー: `invoke('prepare_preview',{path,mode})` → core が comrak(`unsafe_`)→ammonia でサニタイズHTMLを生成し**custom protocol のキャッシュに置く** → フロントは別WebView の `src` を該当URLへ。HTML本体は JS を一切経由しない。
- 差分: `invoke('compute_diff',{path})` → core が baseline＋current を similar で比較 → hunk → フロントの read-only unified レンダラ。

---

## 4. モジュール対応（旧 C++ core/ → Rust pika-core）

旧 `src/core/*` の責務境界を Rust モジュールへ写す。

| 旧 C++ | 新 Rust | 主依存・注記 |
|---|---|---|
| core/document | `document`（読込・エンコーディング・改行・巨大ファイル段階制・表現不能文字検出） | encoding_rs |
| core/workspace | `workspace`（ツリー列挙・除外リスト・自然順・シンボリックリンク循環検出・OneDriveプレースホルダ除外） | std::fs, windows |
| core/watcher | `watcher`（**自前合成層**: デバウンス/合体・rename正規化(FileId補強)・自己保存抑制・オーバーフロー再同期・ポーリング） | notify / windows |
| core/diff | `diff`（Myers＋**文字単位フォールバック**・LF正規化照合） | similar, unicode-segmentation |
| core/snapshot | `snapshot`（ベースライン・退避・content-addressed object・容量GC・自己記述メタ・**厳格DACL**） | zstd, twox-hash, windows |
| core/render | `render`（comrak→ammonia・CSP組立・**暴走ガード入力段計測**） | comrak, ammonia |
| core/search | `search`（fancy-regex/pcre2・ReDoSタイムアウト） | fancy-regex |
| core/settings | `settings`（toml 読取・監視反映・不完全TOMLで直前維持・既定フォールバック） | toml, watcher |
| core/ipc | `src-tauri/ipc`（**自前 named pipe**・引数転送・パス再検証）※アプリ層 | windows |
| （新）CLI | `pika-cli` クレート（console subsystem） | — |
| util | `pika-core` 内 util | — |

`pika-core` は **Tauri 非依存の独立クレート**として cargo workspace に置き `cargo test`。OS依存（windows crate 利用）は core 内のプラットフォームモジュールに閉じ、Tauri/wry には依存しない。

---

## 5. フロントエンド構成（TS）

- ビルド: Vite＋TypeScript。
- UI フレームワーク: **まず vanilla TS でモジュール分割**。タブ/未読・未保存・削除済みの重畳バッジ・3モード×差分トグルの直交状態・通知キューなど状態が複雑なため、重くなった時点で Svelte 導入を早めに見極める（15章）。
- コンポーネント: `ui/tree`・`ui/tabs`・`ui/status`（右下固定・表示専用・差分あり件数）・`ui/notifications`（通知マネージャ）・`editor`（CM6）・`diff`（read-only unified レンダラ）。プレビューは別WebView（6章）。
- **状態の重畳**（要件5.3・11.1）: タブバッジ優先順位（削除済み>未保存>未読）・全タブドロップダウン・隠れ未読バッジ、通知の優先度/集約/スコープ（タブ固有 vs グローバル）/自動消滅を**フロントの状態マネージャ**で実装（19章にチェックリスト）。
- **CM6 の Undo/dirty**: 外部リロードは**単一トランザクション＝1 Undo境界**かつ非dirty（要件5.1）。
- **プレビュー内検索**（要件5.4「プレビューのみ時はプレビュー内テキスト検索」）: 別WebView 内に nonce 付与の信頼済み検索スクリプトを注入してインクリメンタル検索（WebView2 ネイティブ Find が別WebView に効くかは不確実なため自前を基本。15章で検証）。
- **スクロール同期**（要件6.1）: comrak の sourcepos で出力HTMLの各ブロックに `data-line` を付与 → メインWebView ⇔ プレビュー別WebView 間を Tauri event/window message で双方向同期（行ベース近似）。HTML/SVG は同期対象外（要件6.1）。
- **アクセシビリティ**は17章で別建て（ARIA・F6フォーカス・ハイコントラスト・スケーリング）。

---

## 6. プレビュー/差分のセキュリティ境界（全Web化の最重要設計・v2全面改訂）

### 隔離方式: 権限ゼロの別 WebView
未信頼文書（AI出力の HTML/Markdown、`<script>` 含みうる）を、アプリシェル（CM6・Tauri API フルアクセス）と**同一 WebView 内に置かない**。Windows では「親ウィンドウと同一オリジンの iframe は invoke に到達できる」例外（CVE-2024-35222 / GHSA-57fm-592m-34r7 の修正後残存挙動）があり、pika は Windows 専用ゆえこの土俵に乗るため、iframe 隔離では XSS→invoke→実質RCE を構造的に排除できない。

- プレビューは **Tauri の別 WebView（または子ウィンドウ）** に描画し、その WebView ラベルに **capability を一切付与しない**（command ゼロ・`core:default` 不付与・Tauri API 無効）。旧版の「別プロセス WebView2」隔離に最も近い保証。
- 配信は **custom protocol（`pika-preview://`）が Rust から直接**行い、HTML を JS のメインワールドに通さない（3章の IPC 矛盾も解消）。
- ローカル相対参照（画像・CSS）は custom protocol が解決（`file:///` 直接ナビゲートしない＝旧仮想ホスト再現）。**ハンドラは基準ディレクトリへ canonicalize＋prefix 検証**し `..`/絶対パス/シンボリックリンク脱出を拒否。機密ファイル（要件9.1 の `.env`/`*.key` 等）は custom protocol からも配信拒否。

### CSP はレスポンスヘッダで強制（文書内 `<meta>` 依存にしない）
custom protocol レスポンスの HTTP ヘッダで CSP を返し、文書内 `<meta http-equiv>`（CSP/refresh）は ammonia 段で除去する。確定 CSP（既定=外部遮断）:

- **系統A（Markdown/差分/SVG・同梱信頼JSのみ実行）**:
  `default-src 'none'; script-src 'nonce-<rnd>'; style-src 'nonce-<rnd>' 'unsafe-inline'; img-src pika-preview:; font-src pika-preview:; connect-src 'none'; frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`
- **系統B（HTML・文書JS完全無効）**: 上記に加え `script-src 'none'`。別WebView の設定でもスクリプト実行を無効化し、`<meta refresh>` は ammonia で除去（JS 無効でも自動遷移するため）。インラインCSSは許可（要件6.3）。
- **オプトイン緩和**（要件6.2/6.3 の外部リソース許可）は **`img-src`/`font-src` に許可ホストを追加するだけ**に限定し、`script-src`/`connect-src`/`object-src` は緩めない。**既定は必ずオフに戻る**。許可既定値は settings.toml、現在値は state.json（11章）。

### サニタイズの多層と各層の責務
| 層 | 担当 |
|---|---|
| comrak(`unsafe_`) | Markdown→HTML（raw HTML 透過、GFM autolink の URL スキーム制限） |
| ammonia（最終段固定） | タグ/属性/URLスキームのホワイトリスト。`<script>`・`on*`・`javascript:`/`data:`(scriptable)・`<iframe>/<object>/<embed>/<base>/<meta>` 除去。`id`/`name` 制限（DOM clobbering 防止）。**SVG サブセット**（許可するなら `script`/`foreignObject`/`on*`/`xlink:href javascript:` を明示禁止） |
| custom protocol | オリジン分離・CSPヘッダ強制・パス封じ込め・機密配信拒否 |
| 別WebView capability ゼロ | 万一の DOM/描画由来 XSS が成立しても Tauri API へ到達不能 |
| 暴走ガード（Rust render 段） | 要件2.2: 画像6000万px・SVG8000万px/5万要素・HTML10秒を**入力段で計測・超過は配信せず通知バー誘導**（WebView 任せにしない） |

### 信頼済み JS の危険オプション封じ
別WebView へ注入する Mermaid/KaTeX/highlight.js は「未信頼ソースを入力に取る信頼JS」。各ライブラリの XSS 面を名指しで封じる:
- **Mermaid**: `securityLevel:'strict'`（click/任意HTML生成を禁止）。
- **KaTeX**: `trust:false`・`strict:true`・`maxExpand` 制限（`\href{javascript:}`・`\htmlData` 経路封じ）。
- CSP に `'unsafe-inline'`（script）は付けない。nonce のみ。

### 系統A/B 切替の直列化
タブ/モードを素早く切替えた際の競合を防ぐため、**世代カウンタ（タブ,モード,差分）＋ load 完了待ち＋破棄順序**で直列化（ui-design §8 の占有世代と整合）。Mermaid/KaTeX は各ブロック個別描画、構文エラー/タイムアウト（約1秒）で当該ブロックを元コード表示へ戻しエラーマーク（要件6.2）。

---

## 7. Rust と JS のレンダリング責務の線引き

| 処理 | 置き場所 | 注記 |
|---|---|---|
| Markdown→HTML | **Rust** comrak(`unsafe_=true`) | raw HTML 透過。**最終出力は必ず ammonia を最後に通す**（中間生成物を WebView に渡さない）。autolink の URL も ammonia で再チェック。 |
| HTMLサニタイズ・CSP組立 | **Rust** ammonia | 6章のホワイトリスト・SVGサブセット。 |
| 差分計算 | **Rust** similar | 行/grapheme LCS は標準機能。**自前なのは「語境界不成立行→grapheme単位へ切替える判定」のみ**（unicode-segmentation 併用）。LF正規化照合。 |
| 未読/ベースライン照合 | **Rust** | 差分と同じ LF 正規化を共有。 |
| 暴走ガード計測 | **Rust** render | 入力段で px/要素数を判定（要件2.2/12.2）。 |
| Mermaid/KaTeX/highlight | **JS**（別WebView・nonce 注入） | 危険オプション封じ（6章）。 |
| エディタのハイライト | **JS** CodeMirror Lezer | CM6 内在。 |
| unified 差分の描画 | **JS** | Rust 算出 hunk を DOM 描画。**行頭±記号・変更語の下線/太字・前後変更ジャンプ(F8/Shift+F8)・色非依存表現・ハイコントラスト**はフロント実装（要件8.2/11.5）。読み取り専用。 |

---

## 8. 巨大ファイル段階制（要件2.2 の Web 現実値改訂）

CodeMirror 6 はインメモリ文書モデルで Scintilla ほど巨大ファイルに堅くない。要件2.2 の閾値を**実測に基づき Web 現実値へ改訂**する。

### 実測ゲート（15章 Open・実装前必達）
基準機で CM6 の編集体感が劣化し始めるサイズを実測。**「第1段階＝10MB で編集・検索・保存が通常通り可能」を死守できるか**を合否基準にする（中心シナリオ「AI出力の単一行巨大 JSON/JSONL」の閲覧・編集が後退しないため）。10MB を下回って劣化するなら中心体験の後退として別途要件改訂を提案。

### 改訂の波及表（v2 新設）
| 改訂点 | 連動改訂対象 |
|---|---|
| 編集上限を 200MB から実測値へ引下げ | 要件5.4「第2段階(200MB超)読み取り専用・置換無効」の数値 |
| 機能自動オフ閾値（暫定10MB維持） | 要件2.2 第1段階・要件9.2「閾値跨ぎでベースラインobject保持」 |
| 上限(拒否)を 2GB より手前へ下げる可能性 | 要件2.2 上限 |

### 巨大ファイル読み取り
編集上限超は **Rust ストリーミングのバイト範囲読取＋フロントの仮想化ウィンドウビューア**（ログ/JSON ビューア型）で**読み取り専用**閲覧。CM6 に全量ロードしない。**range 配信は custom protocol（HTTP Range 様）か Channel API**（3章の IPCコストと一括で解決）。閲覧・検索のみ（編集/保存/置換不可＝要件2.2 第2段階の精神）。

行長ガード（1行10万字超でハイライト/折返し自動オフ）、暴走ガード（6章・Rust 段で強制）は維持。改訂後の確定値は15章で埋め requirements.md 2.2/5.4/9.2 を改訂（16章）。

---

## 9. CLI・単一インスタンス・データルート・配布

### CLI（二段構成・要件3.1/3.4）
- **`pika-cli.exe`（`#![windows_subsystem="console"]`）**: `--help`/`--version`/引数検証/終了コードを**同期処理**。GUI 起動が必要なときのみ `pika.exe`（`windows_subsystem="windows"`）を spawn。`pika --version > v.txt` のリダイレクト取得・対話シェルでのプロンプト復帰を確実に満たす。
- `-g <file>:<行>:<桁>`（VS Code 互換）のパース規則は要件3.1 のまま（ドライブレターのコロンを分割対象外）。
- 出力エンコーディング（コンソールコードページ/UTF-8）を確定し要件3.4「文字化けしない」を満たす。

### 単一インスタンス＋引数転送（要件3.2）
- **自前 named pipe を既定実装**: `\\.\pipe\pika-<ユーザーSID>`・ユーザー限定 DACL・`PIPE_REJECT_REMOTE_CLIENTS`・受信≤数KB打切り・JSON スキーマ検証・受理操作=パスオープン限定。`CreateNamedPipe` の成否を原子的ロックに（要件3.2・旧 design.md §3 core/ipc 移植）。
- **受信引数は信頼せず core 検証層で再正規化・再検証**（絶対パス化・UNC/ADS/長パス接頭辞の扱い確定・健全性検査）。クライアント側で絶対パス正規化してから転送（要件3.2）。
- tauri-plugin-single-instance は上記全項目を満たす証跡が取れた場合のみ利便部分を流用。満たさなければ不採用。

### データルート解決（要件13）
- 起動最初期に1回解決し全モジュールへ確定パスを渡す: exe 隣に `portable.txt` あれば `./pika-data/`、無ければ `%LOCALAPPDATA%\pika\`（settings は `%APPDATA%\pika\`）。
- state.json/index.json は `version` を持ち未知版は読まず/書かず/再生成せず安全側（要件13）。

### 最近使った項目・ジャンプリスト（要件10.2・v2 で明記）
- 最近使ったファイル/フォルダ各20件を state.json 保持。
- **Windows タスクバーのジャンプリスト**は `windows` crate（`ICustomDestinationList` 等 COM）で実装。

### 配布（要件13）
- Tauri bundler でユーザー単位インストーラー（管理者不要）＋ポータブル zip。
- エクスプローラー統合（要件3.3）は `HKCU\Software\Classes`（`OpenWithProgids`・`shell\pika\command`）を登録/解除、ポータブル版は登録しない。「既定のブラウザで開く」は **スキーム/引数を検証したラッパ command 経由**にし生 `shell-open` を未信頼入力に晒さない。
- **アンインストール時 snapshots はユーザー選択（既定残す）**を bundler（WiX/NSIS）のカスタムページで実装（要件13）。
- 同梱 OSS のライセンス文を installer/zip と About に同梱。コード署名なし・自動更新なし。

### capability マップ（最小権限・v2 新設）
| WebView/ウィンドウ | 付与 capability |
|---|---|
| メインウィンドウ | pika 独自 command の最小集合のみ。`fs`/`path`/`http` plugin を使うなら scope をデータルート/ワークスペース配下に限定 |
| プレビュー別WebView | **権限ゼロ**（command 無し・Tauri API 無効） |

---

## 10. テーマ・アクセシブルな配色

- `ui-mock.html` の CSS 変数トークンを**単一源**として `theme/` に置く。ライト/ダーク/システム追従（既定システム追従）。OS テーマ変更は Tauri のテーマイベントで受けクラス切替、再起動不要。
- エディタ（CM6 テーマ）・ツリー・プレビュー（別WebView へ CSS 変数を渡す）に適用。HTML プレビューは文書スタイル尊重（要件11.3）。
- **ハイコントラスト/強制配色**（要件11.5・2.3）: `forced-colors: active` / `prefers-contrast` で**独自トークンを降格**し OS の `system-color`/`CanvasText` 等に委ねる（トークンがハイコントラストを上書きしない設計）。
- **テキストスケーリング/DPI 追従**（要件11.5・2.3）: 寸法は `rem`/`em` 基準にし、固定 px 多用を避け、WebView2 のズーム/DPI に連動（ui-mock の px 指定箇所は rem 系へ移す＝16章 ui-design 改訂）。
- メニューバーは OS 標準（Tauri ネイティブメニュー）。

---

## 11. スナップショット/状態の保存（要件9・10 の忠実移植）

- 保存先: データルート配下 `snapshots\`（ワークスペースパスのハッシュキー）。**windows crate で明示 DACL（所有者=current user のみ）をデータルート最上位に設定**（ポータブル版が緩い場所に置かれても平文退避が露出しない＝要件9.1 機密最小化）。ワークスペース内に管理ファイルを作らない。
- content-addressed object（twox-hash で重複排除・共有）＋ zstd 圧縮。退避 object に自己記述メタ（元relPath・kind・時刻・元index世代）を併記し index 破損時も object 走査から退避一覧を再生成（原則1）。
- ベースライン（常に1件）／退避（ファイルごと最新10件 LRU＋14日保護＋baseline-replace バッチ）。容量上限既定500MB・90日GC（要件9.2/9.3）。共有 object の物理削除は全参照不在を確認後（要件9.3）。
- 機密ファイルはハッシュのみ（要件9.1）。10MB 境界は「10MB未満のみ内容保存」（要件9.2）。
- **リネーム/移動の未読・ベースライン継承**（要件4.2・9.1）: rename ペア成立判定は watcher 合成層（FileId 補強）。相互スワップ/往復/クロスディレクトリ/上書きrename を安全側で正規化し、引き継ぎ失敗エントリは旧キーで孤立保全（90日GC）。
- **確認済み/巻き戻し**（要件8.3）: 差分時点のディスクスナップショットをベースライン採用、確定直前に mtime/ハッシュ再照合（変化していれば中断・再差分）。「すべて確認済み」は実行開始時点の未読集合をフリーズ、更新前ベースラインを baseline-replace で一括退避。退避不能（10MB以上・画像）は巻き戻し非活性（要件7.3 退避不能ガード）。

### state.json と復元（要件10.1）
- 保存: 開いていたフォルダ・タブ・カーソル/スクロール・表示モード・差分トグル・ツリー展開/収納・ウィンドウ状態。アトミック書込（要件12.1）。
- **復元時のパス検査3分岐**: 消失=「削除済み」表示・別物=未読復元・ワークスペース消失=空状態へ安全遷移（要件10.1）。

### エンコーディング保存中断フロー（要件5.2/5.6）
`save_file` は保存前に現エンコーディングで表現可能かを検査し、不可なら中断して `[UTF-8で保存/該当文字を確認/キャンセル]` をフロントへ返す。改行はバイト列維持（混在改行も統一しない）。Reopen/Change Encoding 導線は「表示」メニュー。

---

## 12. エラー処理・テスト方針

### エラー処理
- コア公開 API は `Result<T, PikaError>`（thiserror）。例外はモジュール内に閉じる。
- command 層は core の `Result` を serializable error へ写し、フロントは Promise reject → 通知バー。
- アトミック書込・クラッシュ耐性・単一インスタンスのロック残留からの正常起動（要件12.1）。

### テスト
- **pika-core を `cargo test` で単体検証**（重点: diff〔日本語の文字単位フォールバック・改行混在・空ファイル〕・watcher〔イベント合成/自己保存抑制/オーバーフロー再同期/rename正規化〕・snapshot〔退避と容量管理〕・エンコーディング往復・render のサニタイズ）。旧 gtest 735 件相当を再建。
- UI 自動テストは初期版では持たず `docs/acceptance.md`／`acceptance-findings.md` の手動チェックリストで代替。将来 Playwright で系統C を部分自動化の余地。
- 性能（起動・メモリ・プレビュー初回・**IPCラウンドトリップ**）を各スプリント末に計測しリリース前ゲート。**常駐リーク**（18時間級ソーク・外部変更1000回・プレビュー50回更新）を Rust 側自己計測で継続監視（要件2.1）。
- **`cargo audit` を CI ゲート**（同梱 crate/JS の CVE 追跡＝要件13 の運用を Rust 側へ拡張）。
- 正規表現は **ReDoS タイムアウト**（マッチ上限・キャンセル可能＝要件5.4）。

---

## 13. リポジトリ構成（新）

```
pika/
├── Cargo.toml                 cargo workspace
├── crates/
│   ├── pika-core/             UI非依存コア（旧 pika_core 相当・cargo test）
│   │   └── src/{document,workspace,watcher,diff,snapshot,render,search,settings,util,platform}/
│   └── pika-cli/              console subsystem の薄いCLI（--help/--version/引数検証/終了コード）
├── src-tauri/                 Tauri アプリ（pika-core 依存）
│   ├── Cargo.toml
│   ├── tauri.conf.json
│   ├── build.rs
│   ├── capabilities/          メイン=最小／プレビューWebView=ゼロ
│   └── src/{main.rs, commands/, ipc/, protocol/, cli.rs}
├── src/                       TS frontend（Vite）
│   ├── index.html
│   └── {main.ts, ui/, editor/, diff/, preview/, theme/, a11y/, styles/}
├── assets/                    同梱JS/CSS（mermaid/katex/highlight）, THIRD_PARTY_NOTICES
├── installer/                 Tauri bundler 設定（snapshots カスタムページ）＋ポータブルzip
└── docs/                      既存（requirements/ui-design 流用、design.md は16章で改訂）
```

旧 C++ `src/` は退避ブランチ（`wip/cpp-core-snapshot` 等）へ保存し本ツリーから除去。

---

## 14. 実装順序（最初の縦切り＝最薄のプラットフォーム証明ループ）

移行の最大リスクは**プラットフォーム結線＋IPCコスト＋watcher 移植**。design.md 14章「技術リスクのスパイクを最初に」に沿う。

1. **スパイク/結線（最薄ループ）**: cargo workspace＋Tauri scaffold → フォルダを開く → ツリー → タブ → CM6 編集 → 保存。**この段で IPC コスト実測**（プレビューHTMLを invoke vs custom protocol 直配信で比較）・データルート解決・テーマトークン・a11y 骨格（ARIA/F6）を立証。
2. **監視**: watcher 自前合成層 → `fs-changed` → 未読バッジ。**notify のオーバーフロー表面化を実機検証**し、不可なら windows crate 直叩きへ。自己保存抑制・rename 正規化を cargo test で再建。
3. **スナップショット/差分/確認済み**: ベースライン → similar 差分（文字単位フォールバック）→ read-only unified ビュー → 確認済み/巻き戻し/退避。**中心体験貫通**。
4. **プレビュー**: 権限ゼロ別WebView＋custom protocol 直配信。系統A（comrak→ammonia＋信頼JS）→ 系統B（HTML・JS無効）→ スクロール同期・プレビュー内検索。Mermaid/KaTeX/highlight 注入（危険オプション封じ）。
5. **CLI/単一インスタンス/状態復元**: 二段構成＋自前パイプ＋`-g`＋state.json 復元3分岐＋ジャンプリスト。
6. **巨大ファイル/エンコーディング/検索置換**: CM6 実測→2.2 閾値確定、仮想化ビューア、fancy-regex 検索置換、Shift_JIS 往復・保存中断。
7. **アクセシビリティ仕上げ・エッジケース・配布**: 17章の a11y 全項目、19章チェックリスト、Tauri bundler。

各段末に性能計測。中心体験（1〜3）優先。

---

## 15. 確定すべき Open 項目（実装前/初期スプリントで埋める・必達）

1. **IPC コスト実測**（段1・必達）— プレビューHTML/保存/diff/巨大ファイルの転送方式を確定し性能目標内を立証。
2. **watcher のオーバーフロー表面化**（段2・必達）— notify が ERROR_NOTIFY_ENUM_DIR 相当を拾うか。握り潰すなら windows crate `ReadDirectoryChangesW` 直叩き。要件7.4「100件同時で取りこぼさない」が前提。
3. **別WebView の権限ゼロ隔離の実証**（必達）— Windows 実機 Release で、プレビュー WebView から `invoke`/`__TAURI_INTERNALS__` 経由の任意 command が到達不能であることを確認。1つでも到達したら設計やり直し。
4. **アクセシビリティの実機検証**（17章）— ナレーター/UIA で主要UI・別WebView 内プレビューが辿れるか。
5. **WebView2 不在時の起動**（18章）— Tauri ウィンドウ生成可否。不可なら最小ネイティブダイアログ経路。
6. **巨大ファイル CM6 実測**（8章）— 「10MB 第1段階で編集通常通り」死守可否 → 2.2/5.4/9.2 改訂値。
7. **プレビュー内検索**（5章）— WebView2 Find が別WebView に効くか／自前検索スクリプトか。
8. **fancy-regex 機能テスト**（2章）— 要件5.4 全通過なら採用、不通過機能のみ pcre2。
9. **単一インスタンス**（9章）— 自前パイプを既定実装。プラグイン流用は要件3.2 全項目の証跡が取れた場合のみ。
10. **フロント UI フレームワーク**（5章）— vanilla TS で骨格、状態が重くなったら Svelte。

---

## 16. 旧 canon の改訂計画（本書承認後に別タスクで実施）

- **requirements.md 1章（技術スタック）**: 本書2章の表へ全面差し替え。アーキ原則「WebView2 はプレビュー/差分のみ／ツリー・タブ・エディタはネイティブ」→「全Web描画・未信頼文書は権限ゼロ別WebViewで隔離」。
- **requirements.md 2.2（巨大ファイル閾値）**: 8章の実測確定値へ。**5.4・9.2 の連動数値も改訂**。
- **requirements.md 2.3（WebView2不在時）**: 「不在でもエディタ/ツリー/監視は動く」→「WebView2 必須前提・不在時は最小ネイティブダイアログで導入案内」（18章）。
- **requirements.md 11.5（アクセシビリティ）**: 「ネイティブコントロール採用の利点で UIA/MSAA」→「全Web UI で **ARIA により主要UIのアクセシブルネームを供給**・F6フォーカス・forced-colors 追従」（17章）。これを書かないと 11.5 が wx 前提で永久未達扱いになる。
- **CLAUDE.md**: 「ネイティブ優先（WebView2 はプレビュー/差分のみ）」削除→「全Web描画・未信頼文書隔離」。「UI系の依存追加は原則禁止」→「UI 依存は CodeMirror 等の必要最小・サイズと天秤」。リポジトリ構成・技術スタック前提（vcpkg/MSVC/wx → Tauri/cargo）を更新。
- **docs/design.md**: アーキ(2章)・モジュール(3章)・実装順序(14章)・構成(12章)を本書に整合。「コアは UI を知らない」「ワークスペースを汚さない」「Result<T>」は**維持**。
- **docs/ui-design.md**: 実装手段直書き箇所（§6 アイコン=wxBitmapBundle、§13 a11y=UIA/MSAA、§2 WebView2注入、固定px寸法）を Web 向けに改訂。視覚仕様（配色トークン・状態記号・レイアウト）自体は流用。
- **docs/acceptance.md／acceptance-findings.md**: ネイティブ前提の所見は新スタックで再検証扱い（手動チェック項目自体は流用）。

---

## 17. アクセシビリティの全Web再構築（v2 新設・要件11.5 の担保）

旧版は wx ネイティブコントロールが UIA/MSAA のアクセシブルネームを**自動供給**した。全Web UI ではこれが消えるため ARIA で再構築する（要件11.5 を満たす唯一の手段）。

| 領域 | 設計 |
|---|---|
| ツリー | `role="tree"/"treeitem"`、`aria-expanded`/`aria-selected`、状態マーク（±/◆/取り消し線）を読み上げテキスト化（`aria-label`） |
| タブ | `role="tablist"/"tab"`、未読/未保存/削除済みバッジの aria 表現 |
| 通知バー | `role="status"`/`aria-live="polite"`（非モーダル通知の読み上げ） |
| ステータス | 右下固定・表示専用。差分あり件数を `aria-label` |
| フォーカス | **F6/Shift+F6 のペイン間循環**を自前フォーカスマネージャで実装（要件11.5・ui-design §13）。キーボードのみで「開く→プレビュー→差分→確認済み」完走 |
| ハイコントラスト | `forced-colors: active`/`prefers-contrast` で独自トークン降格（10章） |
| テキストスケール | rem/em 基準＋WebView2 ズーム連動（10章） |
| エディタ | CodeMirror 6 の ARIA を活用。カスタムガター/差分は別途 aria 付与 |
| プレビュー別WebView | a11y ツリーが分断されるため、ナレーターが本文を辿れるか実機検証（15章 Open 4） |

差分表示は色だけに依存せず +/- 記号併用（要件11.5・8.2）。**初期版の受け入れ基準**として扱う。

---

## 18. WebView2 不在時のフォールバック（v2 新設・要件2.3 改訂）

Tauri はアプリシェル全体が WebView2 上にあるため、不在/破損/最低版未満では**ウィンドウすら描けず**、旧要件2.3「エディタ/ツリー/監視は動く」を構造的に満たせない。

- **要件2.3 を改訂**: WebView2 を必須前提とする。不在/破損時は **Tauri 起動前に最小ネイティブダイアログ（Win32 MessageBox・WebView 非依存）**で Runtime 導入/更新を案内し終了する。
- 全Web化で失う堅牢性（WebView が死ぬと UI 全体が死ぬ）を要件側で受容し、達成水準を再定義する（16章）。

---

## 19. 欠落機能の移植チェックリスト（v2 新設・取りこぼし防止）

実装済み機能のうち、本書本文で薄い箇所を逐条で受ける（要件章番号付き）。

- [ ] 通知バーのキュー運用（11.1）: 優先順位（衝突>設定エラー>外部リソース>JS検知>巨大ファイル）・同一ファイル/種別の集約・タブ固有/グローバル区別・最大3本＋「他N件」・種類別の自動消滅条件 → フロント通知マネージャ（5章）
- [ ] エディタ⇔プレビュー双方向スクロール同期（6.1）→ comrak sourcepos＋WebView間メッセージ（5章）
- [ ] プレビューのみ時のプレビュー内テキスト検索（5.4）→ 別WebView 内検索スクリプト（5章・15章）
- [ ] 最近使った項目＋ジャンプリスト（10.2）→ state.json＋windows crate COM（9章）
- [ ] 状態復元のパス検査3分岐（10.1）→ 11章
- [ ] エンコーディング保存中断フロー・混在改行維持・Reopen/Change Encoding（5.2/5.6）→ 11章
- [ ] rename/移動の未読・ベースライン継承（4.2）→ watcher 合成層＋FileId（4/11章）
- [ ] 外部変更反映: 自動リロードでスクロール/カーソル維持・削除済み表示・リネーム追従（7.2）→ CM6 状態＋11章
- [ ] 衝突: 退避不能ガード（既定ブロック）・rename置換直後Ctrl+S退避（7.3）→ 11章
- [ ] 非テキスト: 画像簡易ビュー＋寸法プリチェック・非対応バイナリの「既定アプリで開く」（12.2）→ render/フロント
- [ ] 診断ログ: ローテーション・内容非記録・ログフォルダを開くメニュー（12.3）
- [ ] FSエッジ: シンボリックリンク循環検出・OneDriveプレースホルダ除外・読み取り専用属性誘導・ネットワークドライブ ポーリング（12.1）→ workspace/watcher（4章）
- [ ] CM6 外部リロードの単一Undo境界・非dirty（5.1）→ 5章
- [ ] ステータス右下固定・表示専用・pointer-events（11.1・ui-design §9）→ 5章
- [ ] 主要ショートカット表・代替割当・Ctrl+Enter 誤爆防止（11.2）→ フロント

---

## 20. 本書のスコープ確認

- 本書は**1つの移行アーキテクチャ**を定義する。個別機能の詳細仕様は requirements.md、視覚仕様は ui-design.md/ui-mock.html が正典で、本書はそれらを Tauri 上でどう実現するかのみを規定する。
- 実装は本書承認後。次の一手（実装プラン作成・スプリント着手）はユーザーが決める。
