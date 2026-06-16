# pika — 開発スプリント仕様（spec.md）

## 出典（source_doc）

本仕様は以下の正典ドキュメントを正として整形したものである（元ドキュメントは書き換えない／Read のみ）。
迷ったら上位の意図に従い、詳細は下位を見る。要件の可否は requirements.md を正とする。

1. `C:\dev\pika_editor\docs\minimal-plan.md` — コンセプト・スコープの根拠
2. `C:\dev\pika_editor\docs\requirements.md` — 要件の正（全14章＋各章「受け入れ基準」）
3. `C:\dev\pika_editor\docs\design.md` — 設計（12章リポジトリ構成・13章テスト方針・14章実装順序・15章判断ガイド）
4. `C:\dev\pika_editor\CLAUDE.md` — プロジェクト指示（設計原則の優先順位・確定済み判断）

設計原則の優先順位（CLAUDE.md / design.md 1章。スプリント設計はこれに従う）:
**データを失わない ＞ 固まらない ＞ 軽い ＞ 足さない ＞ 速く作る**

---

## 目的

Windows 11 x64 向けの超軽量 Markdown/HTML エディタ「pika」を作る。AIエージェントが生成・編集した
テキストファイルを、人間が即時に確認・差分レビューしながら軽く修正するための伴走ツール。VS Code/IDE の
代替ではなく「見る・確認する・少し直す」に集中する。UI言語は日本語のみ、CLIコマンド名は `pika`。

中心体験（最優先で貫通させる縦切り。design.md 14章）:
1. AIエージェントがファイルを生成・編集する
2. エディタが外部変更を即時反映し、変更ファイルをツリー上に未読表示する
3. Markdown/HTML をリアルタイムプレビューする
4. 前回確認時点からの累積差分（既読モデル）を赤/緑で確認し、確認済みにする
5. 必要に応じて人間が軽く修正して保存する

---

## 機能一覧（requirements.md の章に対応。スプリント化対象）

- **ビルド基盤**（design.md 12章）: CMake + vcpkg manifest、ターゲット分割 `pika_core`（wx非依存・`core/`＋`util`）/ `pika`（GUI exe）/ `pika.com`（コンソールスタブ）/ `pika_tests`（gtest）。Debug/Release、`/W4`・警告エラー扱い・Release は `/MT`・`/O2`・LTCG
- **CLI（エージェント向けAPI / req 3章）**: `pika <パス>...`、引数なし=前回状態復元、`pika -g <file>:<行>[:<桁>]`（VS Code互換・ドライブレターのコロン非分割）、`--help`/`--version`。シングルインスタンス（名前付きパイプ `\\.\pipe\pika-<SID>`・DACL＋`PIPE_REJECT_REMOTE_CLIENTS`・JSON引数転送）。終了コード（0=受理、非0=エラー）。相対パスはクライアント側で絶対パス正規化
- **ファイルツリーと未読表示（req 4章）**: フォルダ先行・自然順ソート、既定除外（`.git`/`node_modules`）、未読バッジ（ファイル自身／子孫伝播を区別）、自己保存は未読化しない（保存後ハッシュ一致が必須条件）、リネーム/移動での未読・ベースライン・退避の引き継ぎ、セッション跨ぎ保持
- **エディタ・タブ・検索・保存（req 5章）**: Scintilla（wxStyledTextCtrl）、タブ/スペース原文維持・保存時非正規化、エンコーディング自動判定（BOM優先→UTF-8/Shift_JIS妥当性検査）と往復維持、表現不能文字の保存中断、混在改行の維持、複数タブ（未読/未保存/削除済みの重畳表示優先順位）、検索（PCRE2・後方参照/キャプチャ参照/Unicode文字クラス）・置換、巨大ファイル段階制（10MB/200MB/2GB）・行長ガード
- **プレビュー（req 6章）**: 4モード（プレビューのみ/分割/ソースのみ/差分）、md4c GFM、Mermaid/KaTeX/コードハイライト同梱・遅延読み込み・オフライン、ローカル画像・壊れた参照プレースホルダ、リモートリソース既定オフ（オプトイン）、ホワイトリスト方式サニタイズ＋CSP（`script-src https://app.pika` のみ）、HTMLプレビューは JS無効・仮想ホスト `doc.pika` 経由・`file:///` 直開き禁止、JS検知時の「既定のブラウザで開く」導線
- **外部変更の反映と衝突処理（req 7章）**: `ReadDirectoryChangesW` 監視＋デバウンス100ms・イベント合成、自己保存抑制（ハッシュ一致主条件・ワンショット・時刻窓は補助）、アトミック置換（rename）検知、確定読み（静穏期間＋mtime/サイズ安定）、バッファオーバーフロー再同期、監視不能環境のポーリングフォールバック（5秒）＋F5、クリーン時自動リロード（単一Undo境界・dirtyにしない）、衝突時の退避（conflict/incoming）・通知バー、退避不能ガード（既定ブロック）、自動マージなし
- **差分表示と既読（req 8章）**: 既読ベースライン（初回オープン=全既読、確認済みで更新）、累積差分（前回確認時点→現在）、dtl（Myers）行差分＋単語/文字単位ハイライト、LF正規化後の照合（改行のみの差は出さない）、unified（インライン）表示・+/- 記号併用（色非依存）、確認済み操作・「すべて確認済み」（baseline-replace 退避・一括取消）・ファイル単位の巻き戻し（rollback 退避）、差分は読み取り専用・ワーカー計算・10MB以上自動オフ
- **スナップショット（req 9章）**: データルート配下 `snapshots\<wsKey>\`（`index.json` ＋ `objects\<hash>` zstd圧縮・XXH3重複排除）、ワークスペースを汚さない（`.pika` 等を一切作らない）、機密ファイル（`.env`/`*.key`/`*.pem`/`*secret*`）はハッシュのみ記録、退避の自己記述メタ（index破損時に objects 走査で復元待ち一覧）、容量管理（件数LRU・容量GC500MB・90日GC・未復元退避14日保護・mark-and-sweep）、起動時未読判定（mtime+サイズ→不一致のみハッシュ）、ACL（本人のみ）
- **状態復元・最近使った項目・設定（req 10章）**: `state.json`（窓・タブ・カーソル・スクロール・モード・ツリー展開・テーマ現在値・最近20件）、引数なし起動で完全復元、消失タブの安全遷移、`settings.toml`（`%APPDATA%\pika\`・読み取り専用＝pikaは書き戻さない・即時反映・不正値は既定フォールバック＋警告）、ジャンプリスト
- **UI構成（req 11章）**: メニュー/左ツリー/タブバー/通知バー（最大3本＋「他N件」・優先順位・集約）/メイン領域/ステータスバー、ツールバーなし、ショートカット初期割当（カスタマイズなし）、テーマ（ライト/ダーク/システム追従・HTMLプレビューは非適用）、アクセシビリティ（色非依存・F6フォーカス移動・ハイコントラスト・スクリーンリーダ）
- **エッジケース・エラー処理（req 12章）**: 読み取り専用/権限なし/シンボリックリンク循環/ネットワークドライブ/クラウドプレースホルダ（オフライン属性除外）/フォルダ削除/クラッシュ耐性（アトミック書き込み）/ロック残留、画像簡易ビュー（wxImage・WebView2不使用・ピクセル数ガード）、診断ログ（内容を書かない）
- **配布・インストール（req 13章）**: ユーザー単位インストーラー（Inno Setup）＋ポータブルzip、データルート解決（`portable.txt` で `./pika-data/` 切替・既定 `%LOCALAPPDATA%\pika\`）、エクスプローラー統合（`HKCU\Software\Classes`・オプトアウト・ポータブルは非登録）、state/index の version 単調増加・未知versionは安全側、サードパーティライセンス同梱、コード署名なし（SmartScreen回避手順）、自動更新なし

---

## 非対象（初期版でやらないこと。req 14章＋minimal-plan.md 14章。スプリント対象外として保全）

スプリント化しないが、要望に含まれていた要素を情報落ちさせないためここに残す。実装中にこれらを
足したくなったら、実装せず要件改訂を提案する（design.md 15章・CLAUDE.md 設計原則「足さない」）。

- IDE機能 / Gitクライアント / Git差分・Git状態管理 / 内蔵AI / AIチャット / プロジェクト管理 /
  ビルド・実行・デバッグ / 本格コード編集 / 複雑な履歴管理・永続的バージョン管理 / 自動バックアップ
- 任意JavaScript実行（ユーザー文書由来のJS。pika同梱の信頼済みJS〔Mermaid/KaTeX/ハイライト〕は別扱い）
- プラグイン機構（公式・外部とも。取捨選択は内部モジュール＋設定トグルで実現）
- WYSIWYG編集（ソース編集＋リアルタイムプレビューを優先）
- フォルダ横断検索（grep）
- ホットエグジット（未保存のまま終了・復元）
- GUI設定画面（settings.toml 直編集で代替）
- 自動更新
- サイドバイサイド差分 / レンダリング済みプレビュー上の差分
- 「次の未読ファイルへ」ジャンプ
- 複数ウィンドウ・複数フォルダ同時オープン
- CLI の `--wait` オプション
- キーボードショートカットのカスタマイズ
- 永続Undo
- 画像の編集・変換
- 日本語以外のUI言語（文言リソース一元管理で将来対応の余地のみ確保）
- ARM64ネイティブビルド（x64エミュレーションでの動作は妨げない／検証対象外）
- 外部 `.css` ファイルの読み込み・反映（資料用途AI生成HTMLは自己完結が規約のため）
- セクション（見出し）単位の既読 / ハンク単位の採用・却下のような高度レビュー / ハンク採否
- サンドボックス付きJS実行 / MCP等のプロトコル連携 / セクション単位既読（いずれも将来検討）

---

## 補完した判断（元ドキュメントの不足・現状制約に対し、推測せず planner が明示した判断）

正典ドキュメントは「どう作るか」までは定めるが、**スプリント分割と verify 手段の確定は次フェーズ**
（design.md 14章「スプリント分割は次フェーズで行う」）。本仕様で以下を補完する。後からのドリフト追跡用に
根拠を残す。

1. **現状リポジトリには実装基盤が存在しない**。実在するのは `docs/`・`CLAUDE.md`・`.claude/`・
   `.github/`・`.clang-format`・`.gitattributes`・`.gitignore` のみで、`CMakeLists.txt`・`vcpkg.json`・
   `vcpkg-configuration.json`・`src/`・`tests/` はいずれも未作成（Glob で確認済み）。したがって
   **sprint 1 はビルド基盤の整備から始める**（design.md 12章のターゲット分割・vcpkg manifest を実体化し、
   最小の `pika_core`＋`pika_tests` が ctest で通る最小構成を作る）。これは design.md 14章の「技術リスク
   スパイク」より前に必要な前提であり、リスクスパイクは sprint 2 以降に置く。

2. **重量依存（wxWidgets/WebView2）の vcpkg 初回ビルドは run-dev の verify timeout（15分/コマンド）を
   超えるリスクが高い**。そこで verify を二系統に分ける（design.md 13章「自動単体テストの対象は core/・
   util」「UIの自動テストは初期版では持たない」と整合）:
   - **core-test 系（各スプリントの必須 verify）**: wx非依存の `pika_core`＋`util` と `pika_tests`（gtest）
     を対象にした軽量プリセット `x64-core-test` を用意し、`ctest --preset x64-core-test` を必須 verify と
     する。依存は md4c・dtl・pcre2・zstd・xxhash・toml11・gtest のみ（いずれも小サイズの C/C++ ライブラリ
     で wx/WebView2 を含まない）。これが「ループが進むほど合否判定が決定論側に寄る」担保。
   - **full 系（GUI スプリント。verify 必須に含めない）**: wx を含む `x64-release` プリセットでの
     `pika` exe ビルドは、初回の重量依存ビルドが timeout を超えうるため **acceptance_criteria の must に
     しない**。GUI 実機確認は should、または `docs/acceptance.md` の手動チェックリスト側へ寄せる。
   - sprint 1 の verify には full ビルド到達性を **should** に留め、必須は core-test の成立とする。

3. **技術リスク3点のスパイク（design.md 14章: (a) wx＋Scintilla＋WebView2 共存と起動500ms・
   `TrySuspend` 再表示300ms、(b) watcher のイベント合成・自己保存抑制・バッファオーバーフロー再同期、
   (c) WebView2 の仮想ホスト＋サニタイズ＋JS有効/無効の高速切替の順序保証）は、verify を「該当コア
   ロジックの gtest が通る」「UI非依存で単体テスト可能」に寄せる**。(b)（watcher）と (c) のサニタイズ・
   検知判定（`HtmlInspector`・`MarkdownRenderer`）・CSP組み立ては wx非依存のコアに切り出せるため
   `pika_tests` で決定論検証できる（must）。(a) と (c) のうち WebView2 実機での順序保証・Resume
   レイテンシは GUI 実機が要るため should / 手動チェックリスト側へ（補完判断2に従う）。

4. **verify コマンドは Windows/PowerShell・Bash 双方から実行されうる**。CMake プリセット
   （`cmake --preset` / `cmake --build --preset` / `ctest --preset`）はシェル非依存で同一に動くため
   これを採用する（.claude/settings.json で既に allow 済み）。既存の `.github/workflows/ci.yml` も
   `x64-release` プリセット・`pika_core`/`pika_tests` 分割を前提にしているため、本スプリント設計は
   それと整合する（CI 雛形の `build-test` ジョブの `if: false` を将来外せる構成にする）。

5. **編集直後の post-edit hook 制約**: JSON は `JSON.parse`、C++ は `clang-format --dry-run --Werror`
   で検査される（`.claude/hooks/post-edit-check.mjs`）。生成する C++ は `.clang-format`（Microsoft 基点・
   ColumnLimit 100・IndentWidth 4・UseTab Never）に整形済みであること、JSON は厳密 JSON（コメント不可）
   であることをスプリント実装の前提とする。

---

## 検証手段

### use_playwright: false

**根拠**: pika はネイティブ GUI（wxWidgets）アプリであり、外部公開される **Web UI を持たない**。
プレビュー/差分は WebView2 を内部利用するが、これは外部公開 Web アプリではなく、Playwright の対象に
ならない。したがって `use_playwright: false` を `sprints.json` にも明記する。
GUI/プレビューの実機確認は `docs/acceptance.md` の手動チェックリストで代替する（design.md 13章
「UIの自動テストは初期版では持たない」）。

### 自動検証（各スプリントの verify）

- **必須（決定論・足切り）**: `ctest --preset x64-core-test`（`pika_tests` の gtest。exit code 0 で合格）。
  対象は `core/`＋`util`（wx非依存）。重点は design.md 13章のとおり diff・watcher（イベント合成/自己保存
  抑制/オーバーフロー再同期）・snapshot（退避と容量管理・index破損復元）・エンコーディング往復・render の
  サニタイズ。
- **構成チェック（軽量）**: `cmake --preset x64-core-test`（core-test プリセットの構成が通ること。
  vcpkg manifest の解決＋CMake 構成成立で合格）。
- **整形（編集直後 hook ＋ CI format ジョブ）**: `clang-format --dry-run --Werror`（C++）・JSON syntax。
  これは hook が即時に行うため verify に重ねない。

### 手動検証（should / acceptance.md 側）

- wx＋Scintilla＋WebView2 の共存・起動500ms・`TrySuspend` 再表示300ms（性能ゲート。design.md 11章）。
- プレビュー/差分の WebView2 実機表示・JS有効/無効切替の順序保証の実機確認。
- requirements.md 各章「受け入れ基準」を `docs/acceptance.md` のチェックリストへ写経し、リリース前に実施。
- 性能（起動時間・メモリ）は各スプリント末に計測し劣化を即検知（design.md 14章3）。
