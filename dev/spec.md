# pika — UI実装フェーズ 開発スプリント仕様（spec.md）

## 出典（source_doc）

本仕様は以下の正典ドキュメントを正として整形したものである（元ドキュメントは書き換えない／Read のみ）。
要件の可否は requirements.md を正とし、迷ったら上位の意図に従う。

1. `C:\dev\pika_editor\docs\requirements.md` — 要件の正（全14章＋各章「受け入れ基準」）。本フェーズで主に効くのは 4・5・6・7・8・10・11・12章
2. `C:\dev\pika_editor\docs\design.md` — 設計（2章レイヤー依存・5章主要フロー・6章WebView2・10章UI補足・11章性能・13章テスト方針・14章実装順序）
3. `C:\dev\pika_editor\docs\ui-design.md` — **UI視覚仕様の正典**（配色トークン・タイポ・状態記号〔差分あり ±／新規 ◆／削除済み 取消線〕・ファイルタイプアイコン・ステータス右下固定・差分3モード+トグルUI・15章ビュー別5状態）
4. `C:\dev\pika_editor\docs\ui-mock.html` — モック実体
5. `C:\dev\pika_editor\CLAUDE.md` — プロジェクト指示（設計原則の優先順位・確定済み判断）
6. `C:\dev\pika_editor\dev-core-2026-06-16\spec.md` / `report.md` — **前回（コア層）run-dev の到達点**。何が実装・テスト済みかの基準（土台）

設計原則の優先順位（CLAUDE.md / design.md 1章。スプリント設計はこれに従う）:
**データを失わない ＞ 固まらない ＞ 軽い ＞ 足さない ＞ 速く作る**

---

## 目的

Windows 11 x64 向けの超軽量 Markdown/HTML エディタ「pika」の **UI層・アプリケーション層・プラットフォーム層**を
実装する。前回 run-dev で wx 非依存のコアサービス層（`src/core/` ＋ `src/util`）は決定論的に固まった
（gtest 373 件 PASS）。本フェーズはそのコア API の上に、ユーザーが直接触る GUI と、それを駆動する
アプリケーション層（controller / ViewModel / 状態機械）、および Win32 I/O 実体（プラットフォーム層）を積む。

中心体験（最優先で貫通させる縦切り。design.md 14章2）:
1. **開く** — フォルダ/ファイルを開き、ツリーに表示・タブで開く（CLI `pika <path>` / 単一インスタンス転送）
2. **外部変更を反映** — AIエージェントの編集を `ReadDirectoryChangesW` 監視で即時反映し、ツリーに差分あり（未読）マーク
3. **差分** — 前回確認時点からの累積差分を WebView2 で赤/緑＋記号表示（3モード × 差分トグル直交）
4. **確認済みにする** — ベースラインを更新して差分マークを解除（退避が最後の砦）
5. 必要に応じて人間が軽く修正して保存する（Scintilla 編集・エンコーディング往復・衝突退避）

---

## 検証戦略（本フェーズの核。verify 二系統 — 補完判断1で根拠を詳述）

ユーザーの明示方針「ビジュアルは最後に回す／可能な範囲でゲートをチェック」を次の二系統に具体化する。

### 系統A：決定論ゲート（各スプリントの **must verify**・主要品質シグナル）

UI に依存しないアプリケーション層ロジック（controller / ViewModel / 状態機械）を **wx 非依存のユニット**として
`src/controller/` に切り出し、`pika_core`（wx 非依存静的lib）に含めて `pika_tests`（gtest）から検証する。
対象例（design.md 2章・10章・ui-design.md 5/8章から導出）:

- **タブモデル / TabManager 状態機械** — 重畳状態の表示優先（削除済み ＞ 未保存 ＞ 差分あり。ui-design 5章）、
  タブ追加/削除/アクティブ遷移、消失タブの安全遷移（design 5.1手順4）
- **通知バー集約 ViewModel** — 最大3本＋「他N件」、優先順位（衝突 ＞ 設定エラー ＞ 外部リソース ＞ JS検知 ＞ 巨大ファイル）、
  同一ファイル・同一種別の最新集約、タブ固有/グローバルの切替（design 10章 J1）
- **テーマ解決** — ライト/ダーク/システム追従の解決結果・トークン解決（ui-design 2/12章）、`state.json.theme.current` への保持
- **差分モード状態機械** — (モード∈{ソース,分割,プレビュー}) × (差分ON/OFF) の直交組合せ、占有世代 (タブ,モード,diffOn) の算定、
  巨大ファイル/WebView2不在/ベースライン未取得での差分トグル自動無効化＋理由（ui-design 8章・design 6章）
- **ツリー → ViewModel 変換** — `build_tree()`/`UnreadSet` の結果を状態マーク（±/◆/取消線・伝播 ±淡）と種別アイコン分類へ写像（ui-design 5/6章）
- **状態復元の組み立て** — `state.json` の AppState からタブ/ツリー展開/モード/差分/ペイン収納を再構成（design 7章・5.1）
- **ショートカット割当表** — フォーカス別ディスパッチ表（Ctrl+Enter＝差分/プレビュー時、Ctrl+Shift+Enter＝エディタ時、Ctrl+Alt+Enter＝一括。design 10章 J3）
- **CLI/データルート解決ロジック** — `portable.txt` 検出によるデータルート分岐（純粋関数化。design 5.1・7章 K1）

→ **must verify**: `ctest --preset x64-core-test`（exit 0 で合格）。ループが進むほど合否判定が決定論側に寄る担保。

### 系統B：GUI 配線ビルドゲート（GUI スプリントの **must verify**・ユニットテスト不能部分）

wx/Scintilla/WebView2 の実配線（`src/ui/`・`src/app/` の wx 依存部）はユニットテストできないため、
**コンパイル＋リンク成立**（`/W4`・警告エラー扱い）をゲートにする。wx・WebView2 は vcpkg 導入済み・ビルド済み
（`build/x64-release` に `pika.exe` 生成済み・依存1.5GBキャッシュ済み）のため増分ビルドは高速で、verify timeout（15分/コマンド）に収まる。

→ **must verify（GUI 配線スプリントのみ）**: `cmake --build --preset x64-release`（exit 0＝コンパイル＋リンク成功）。
必須の構成成立確認には `cmake --preset x64-release` も併用する。

### 系統C：視覚・挙動の手動確認（must verify に載せない。should / 最終スプリント手動項目）

ウィンドウ描画・Scintilla 編集・WebView2 実描画・ライブリロード実動・起動500ms・TrySuspend 再表示300ms 等は
GUI 実機が要るため、**フェーズ最後に `docs/acceptance.md` の手動チェックリストへ集約**する（design 13章「UI自動テストは初期版で持たない」）。

### use_playwright: false

**根拠**: pika はネイティブ GUI（wxWidgets）アプリで、外部公開される **Web UI を持たない**。プレビュー/差分は
WebView2 を内部利用するが外部公開 Web アプリではなく Playwright の対象外。`sprints.json` にも `use_playwright: false` を明記する。

---

## 機能一覧（requirements.md の章に対応。本フェーズでスプリント化する範囲）

- **アプリ起動・CLI・単一インスタンス・データルート（req 3章 / design 5.1）**: `main_gui.cpp`（現570B/22行スタブ）を実体化。
  データルート解決（`portable.txt`）、CLI `-g` 受領（コア `parse_argv` 利用）、名前付きパイプ `\\.\pipe\pika-<SID>` の
  実 I/O（`CreateNamedPipe` を原子的ロックに・敗者はクライアント転送）、表示前のサーバー公開で TOCTOU 回避
- **メインウィンドウ・レイアウト・テーマ（req 11章 / design 2.1・10章 / ui-design 2/7/12章）**: `MainFrame`（メニュー/左ツリー/タブバー/通知バー/メイン/ステータス右下固定）、
  テーマ適用（ライト/ダーク/システム追従・`wxSysColourChangedEvent` 再適用）、文言の単一メッセージ定義（ID→日本語）
- **ファイルツリーと未読表示（req 4章 / design 10章 / ui-design 5/6章）**: `wxDataViewCtrl` 第一候補（種別アイコン＋状態マーク共存）、
  逐次追加列挙、シンボリックリンク循環検出、状態マーク（±/◆/取消線・伝播 ±淡）、種別アイコン（lucide 線・`wxBitmapBundle`）、Delete＝ごみ箱（`IFileOperation`）
- **エディタ・タブ・検索・保存（req 5章 / design 5.3・5.7・10章）**: Scintilla（`wxStyledTextCtrl`）配線、`wxAuiNotebook`＋カスタム `wxAuiTabArt`（状態記号描画）、
  エンコーディング/改行の往復維持（コア結果を UI 反映）、保存（衝突検知→incoming退避→アトミック置換）、検索/置換 UI（コア `SearchEngine` 配線）
- **プレビュー（req 6章 / design 6章 / ui-design 8/11章）**: 共有1枚 WebView2、仮想ホスト `app.pika`/`doc.pika`（カスタムリソースハンドラ）、
  CSP/サニタイズ（コア `render_markdown`/`HtmlInspector` 配線）、JS有効/無効の直列切替、ナビゲーションインターセプト、スクロール同期、TrySuspend アイドル回収
- **外部変更の反映と衝突処理（req 7章 / design 5.2）**: Win32 監視スレッド（`ReadDirectoryChangesW`→`RawEvent`→コア `WatcherCore`）、
  ポーリングフォールバック・F5、クリーン時自動リロード（単一Undo）、衝突時の退避・通知バー
- **差分表示と既読（req 8章 / design 5.4・8章 / ui-design 8/11章）**: 差分HTML 表示（コア `DiffEngine`＋`PreviewBuilder` 配線）、
  3モード×差分トグル、確認済み・すべて確認済み・巻き戻し（コア `ReviewFlow` 配線）
- **状態復元・最近使った項目・設定（req 10章 / design 5.1・5.6・7章）**: 起動時 `state.json` 復元、消失タブ安全遷移、
  `settings.toml` 監視反映（コア `core/settings` 配線。読み取り専用）、ジャンプリスト、フォルダ切替
- **エッジケース・画像簡易ビュー（req 12章 / design 10章）**: ラスター画像の `wxImage` 簡易ビュー（WebView2 不使用・ピクセル数ガード）、
  読み取り専用/権限/ネットワークドライブ/クラウドプレースホルダ（オフライン属性除外）の縮退表示、診断ログ（内容を書かない）

---

## 非対象（本フェーズで実装しないもの。情報落ち防止のため保全）

### コアは完了済み（再計画・再実装しない）

`src/core/`（document/workspace/watcher/diff/snapshot/render/search/settings/state/ipc）と `src/util` は
前回 run-dev で完了（gtest 373 PASS）。本フェーズはその上に積むのみ。コアの公開 API（`Result<T>` 方式・
コールバック/イベントキュー I/F）を呼ぶ側を作る。コアのロジック自体は触らない。

### 要件14章「やらないこと」（足さない。実装したくなったら要件改訂を提案）

IDE機能 / Gitクライアント / 内蔵AI・AIチャット / プロジェクト管理 / ビルド・実行・デバッグ / 本格コード編集 /
任意JavaScript実行（ユーザー文書由来。同梱 Mermaid/KaTeX/ハイライトは別扱い）/ プラグイン機構 / WYSIWYG編集 /
フォルダ横断検索（grep）/ ホットエグジット / GUI設定画面（settings.toml 直編集で代替）/ 自動更新 /
サイドバイサイド差分・レンダリング済みプレビュー上の差分 / 「次の未読ファイルへ」ジャンプ /
複数ウィンドウ・複数フォルダ同時オープン / CLI `--wait` / ショートカットのカスタマイズ / 永続Undo /
画像の編集・変換 / 日本語以外のUI言語 / ARM64ネイティブビルド / 外部 `.css` 読み込み / セクション単位既読・ハンク採否。

### 本フェーズの自動 verify に載せない（GUI 実機が要る・系統C へ寄せる）

- 起動500ms・`TrySuspend` 再表示300ms 等の性能ゲート（design 11章。各スプリント末に計測・最終スプリントで `docs/acceptance.md` 化）
- WebView2 実描画・JS有効/無効切替の順序保証の実機確認・Scintilla 実編集・ライブリロード実動・画像簡易ビュー実描画
- 性能ソークテスト（8時間稼働）・配布インストーラー（Inno Setup）・エクスプローラー統合の実機検証

---

## 補完した判断（元ドキュメントの不足・現状制約に対し、推測せず planner が明示）

正典は「どう作るか」までを定めるが、**UI フェーズのスプリント分割と verify 手段の確定は本フェーズの planner の責務**
（design 14章「スプリント分割は次フェーズで行う」）。後からのドリフト追跡用に根拠を残す。

1. **verify 二系統の根拠**。重量依存ビルド（wx/WebView2）は既に済んでおり増分は高速だが、GUI 配線は wx 依存で
   ユニットテストできない。そこで (A) wx 非依存に切り出せるアプリ層ロジックは `ctest --preset x64-core-test`（gtest）で
   決定論検証＝**must**、(B) wx 依存の配線は `cmake --build --preset x64-release`（/W4・警告エラー）の**コンパイル＋リンク成立**で
   ＝**must（GUI 配線スプリントのみ）**、(C) 視覚・実挙動は **手動（should / 最終スプリント手動項目）** とする。
   ユーザー明示方針「決定論ゲートを最大化／ビジュアルは最後」と design 13章「UI自動テストは持たない＝controller を厚くする」に整合。

2. **controller を gtest 対象にする構成変更が前提**。現状 `src/controller/` は存在しない。これを新規作成し、
   wx 非依存ロジックを `pika_core` 静的lib（`PIKA_BUILD_GUI` の影響下に置かない）に含め、`pika_tests` からリンクできるよう
   `src/CMakeLists.txt`・`tests/CMakeLists.txt` を更新する。`pika_core` の include は既に `src/` 全体 PUBLIC のため相互参照は可能
   （調査で確認）。この構成整備自体を sprint 1 に含める。

3. **プラットフォーム層は「設計済み・実 I/O 配線待ち」**。調査で確認した現状（実コード）:
   - `core/watcher` は `RawEvent` 入力 I/F（`WatcherCore::on_raw`）まで実装済みだが、`ReadDirectoryChangesW` を回して
     `FILE_NOTIFY_INFORMATION`→`RawEvent` へ変換する**監視スレッド実体は未配線**（`src/ui` or `src/app` のプラットフォーム層が担当）。
   - `core/ipc` は SDDL/ACL・パイプ名生成・メッセージスキーマ・検証まで実装済みだが、`CreateNamedPipe`/`ReadFile` の
     **実パイプ I/O は未配線**（`src/app` が担当）。
   - `main_console.cpp`（110行）は実装完了（CLI 検証・終了コード）。`main_gui.cpp`（22行）はスタブ。
   未配線分を本フェーズのスプリントに含める。これらの I/O 実体は実 FS/実 OS が要りユニットテスト不能なため系統B（ビルド成立）＋系統C（手動）で検証する。

4. **持ち越し high（前回 report.md）への目配り**。前回 report の持ち越し未解消 facet（#5 object削除と index.json 保存の
   非原子性＝ジャーナリング要・#1 watcher 読取失敗の保留種別）は **コア層の構造変更**であり本フェーズのスコープ外（コア再実装しない）。
   ただし退避結合フロー（`ReviewFlow`×`SnapshotStore`）を controller から呼ぶ際は、その戻り値（`Result<T>`）を握り潰さず
   通知バー/ログへ変換することを controller 側の must criteria に含め、「データを失わない」最上位原則の UI 側での毀損を防ぐ。

5. **編集直後 post-edit hook 制約**。生成する C++ は `.clang-format`（Microsoft 基点・ColumnLimit 100・IndentWidth 4・UseTab Never）に
   整形済み、JSON は厳密 JSON（コメント不可）であること。verify には重ねない（hook が即時実施）。

6. **review-profile の調整**。UI フェーズは frontend-ui・ux の比重が上がる（GUI/ViewModel/状態機械を毎スプリント触る）。
   WebView2 のサニタイズ/CSP 配線では security、起動・200ms 非ブロック・TrySuspend では performance、状態永続化・退避結合では data が効く。
   コスト最適化のため `always` は作らず conditional 中心に倒すが、ローカル個人ネイティブツールの性格（公開 API なし→backend-api=off）は維持する。
   詳細は `dev/review-profile.json` に記す。

---

## 検証手段（まとめ）

### use_playwright: false（上記「検証戦略」参照）

### 自動検証（各スプリントの verify）

- **必須・決定論（系統A）**: `ctest --preset x64-core-test`（`pika_tests` の gtest。exit 0 で合格）。controller/ViewModel/状態機械の wx 非依存ユニットが対象。
- **必須・GUI 配線（系統B。GUI 配線スプリントのみ）**: `cmake --build --preset x64-release`（/W4・警告エラー扱い。exit 0＝コンパイル＋リンク成功）。
- **構成成立（軽量）**: `cmake --preset x64-core-test`（core-test 構成成立）。GUI スプリントでは `cmake --preset x64-release` も。
- **整形（編集直後 hook）**: `clang-format --dry-run --Werror`（C++）・JSON syntax。hook が即時実施するため verify に重ねない。

### 手動検証（系統C。should / `docs/acceptance.md` 側）

- wx＋Scintilla＋WebView2 共存・起動500ms・`TrySuspend` 再表示300ms（性能ゲート。design 11/14章）。
- プレビュー/差分の WebView2 実描画・JS有効/無効切替の順序保証・ライブリロード実動・画像簡易ビュー実描画。
- requirements.md 各章「受け入れ基準」を `docs/acceptance.md` のチェックリストへ写経し、リリース前に実施。
- 性能（起動時間・メモリ）は各スプリント末に計測し劣化を即検知（design 14章3）。
