# run-dev レポート — pika UI 実装フェーズ

- 対象: pika（Windows 向け超軽量 Markdown/HTML エディタ）の **UI 実装フェーズ**（controller/UI/プラットフォーム層）
- 期間: 2026-06-16
- 結果: **全 8 スプリント passed / status = done**
- 検証方針: ビジュアルは最後に回し決定論ゲートを最大化。controller/ViewModel は `ctest --preset x64-core-test`（gtest）で決定論検証、GUI 配線は `cmake --build --preset x64-release`（/W4・warnings-as-errors）のコンパイル+リンク成立をゲート、視覚・実挙動は sprint8 の `docs/acceptance.md`（系統C 手動チェックリスト）に集約。
- verify はオーケストレーターが毎ターン実行し、generator/evaluator の自己申告を D-verify に用いていない。

## スプリント結果表

| sprint | 内容（要約） | ターン数 | 最終 score | passed | 未達 criteria |
|---|---|---|---|---|---|
| 1 | controller 層を pika_core(wx非依存) に新設 + TreeViewModel + ビルド構成 | 1 | 100 | ✅ | なし |
| 2 | 『開く』アプリ層（AppController/データルート解決/CLI/TabManager 状態機械） | 1 | 100 | ✅ | なし |
| 3 | 『開く』GUI/プラットフォーム配線（MainFrame/wxDataView/Scintilla/名前付きパイプ） | 1 | 95 | ✅ | なし（high 3件は polish 送り） |
| 4 | 『外部変更を反映』WorkspaceController + Win32 ReadDirectoryChangesW 監視 | 1 | 90 | ✅ | なし（high 5件は polish 送り） |
| 5 | 『差分』差分モード状態機械 + WebView2/CSP/JS 有効無効切替 | 2 | 92 | ✅ | iter1 で fail(86)→iter2 で解消 |
| 6 | 『確認済み』と保存・衝突退避（ReviewFlow × SnapshotStore） | 1 | 94 | ✅ | なし（high 6件は polish 送り） |
| 7 | 周辺機能（検索/置換 UI・状態復元・settings.toml 監視反映・通知バー集約） | 1 | 97 | ✅ | なし（high 1件は polish 送り） |
| 8 | エッジケース縮退・ビュー別5状態 ViewModel・手動 acceptance 整備（系統C） | 1 | 98 | ✅ | なし |

- **総ターン数: 9**（sprint5 のみ 2 イテレーション、他は各 1）
- **総所要時間（history 集計）: 約 7,440 秒 ≒ 2 時間 4 分**
- 上限（sprint 毎 3・総 15）には未到達。

## 多角レビュー結果

**critical findings は全期間ゼロ**（全ターンの全 reviewer で `has_critical=false`）。high はあったが本ループのゲート上 passed を阻害せず、polish として持ち越し（下記）。

| sprint.iter | 起動 reviewer（critical / high） |
|---|---|
| 1.1 | frontend-ui(0/0), product(0/0) |
| 2.1 | data(0/0), frontend-ui(0/0) |
| 3.1 | frontend-ui(0/1), security(0/1), ux(0/1) |
| 4.1 | data(0/2), frontend-ui(0/3), security(0/0), ux(0/0) |
| 5.1 | frontend-ui(0/1), performance(0/1), security(0/1) ← fail(86) |
| 5.2 | frontend-ui(0/1) |
| 6.1 | data(0/2), frontend-ui(0/2), ux(0/2) |
| 7.1 | frontend-ui(0/0), product(0/0), security(0/0), ux(0/1) |
| 8.1 | frontend-ui(0/0), security(0/0), ux(0/0) |

### review-profile.json 最終構成

- `frontend-ui = always`（UI 層/ViewModel/状態機械を毎スプリント実装するため常時起動。視覚仕様 ui-design.md との整合・状態記号の色非依存弁別を継続監視）
- `security = conditional`（WebView2 サニタイズ/CSP・JS 切替・doc.pika パストラバーサル・パイプ DACL・機密ファイル扱いを触る render/preview/ipc スプリントで起動：実績 s3・s5・s7・s8）
- `data = conditional`（『データを失わない』。退避結合・state.json/index.json version 安全側・確認済み/巻き戻し/保存衝突を触るスプリントで起動：実績 s2・s4・s6）
- `ux = conditional`（中心体験の操作フロー・縮退時の次の一手・キーボード完結を触るスプリントで起動：実績 s3・s4・s6・s7・s8）
- `performance = conditional`（起動 500ms・200ms ブロック禁止・ワーカー実行を触るスプリントで起動：実績 s5）
- `product = conditional`（要件 14 章逸脱の混入確認：実績 s1・s7）
- `backend-api = off`（外部公開 API 契約が無い＝内部 IPC のみ）

## 持ち越し（passed 済み・post-loop / 実 I/O 配線・系統C 手動で解消する high/medium）

a11y と data 整合が再指摘軸。sprint8 の `view_state` / `degrade_model` ViewModel が**色非依存弁別と縮退提示の構造的土台**を提供済みで、残るのは GUI への結線と視覚確認（系統C）。

### a11y（複数スプリントで再指摘 / sprint8 で土台化、最終結線+視覚確認は系統C）
- ファイルツリーの状態マーク（差分あり ±／新規 ◆／削除済み 取消線）の**アクセシブルネーム/取消線が `GetValue` で未配信**＝スクリーンリーダーに意味が伝わらない（s3・s4 frontend-ui）。`docs/acceptance.md` G6/E6 に手動項目として予約。
- 外部削除で消失したタブの**『削除済み』表示が未結線**（s4 frontend-ui）。
- 伝播 ± と実心 ± の**色非依存弁別**（色だけで区別不能）（s4 frontend-ui）。

### data / 整合
- `pipe_server` の `current_user_sid()` 失敗時 **fail-open**（owner-only DACL 黙殺）→ **fail-closed 化推奨**（s3 security high）。
- `apply_renames` が core 公開の `reevaluate`/`orphaned` を**握り潰し**（s4 data）。空ベースライン resync フラッディングは sprint6 のベースライン供給で構造的に解消見込み（最終確認は系統C）。
- 確認済み/一括確認/巻き戻し/衝突保存の**永続化コミット境界**・巻き戻しの `Result` 握り潰し防止（s6 data）。`DocumentController` で退避 `Result` を握り潰さない実装は反映済み、永続化境界の最終観測は系統C。

### 差分/プレビュー（sprint7 テーマ解決で構造解消・最終確認は系統C）
- HTML プレビューに `preview.css` の `html,body` 指定が漏れ**文書スタイルを上書き**（要件 11.3 違反、s5 frontend-ui）→ sprint7 テーマ解決でカバー見込み。
- テーマが `prefers-color-scheme` 依存でネイティブ選択と非同期になりうる／差分行の横スクロール受け皿欠如（s5）。

### sprint8 で新規に出た medium/low（次の polish 候補）
- **medium** `view_state.cpp` の **第4 Empty ケース**（フォルダは開いているが空フォルダ等で項目なし）が `EmptyReason::None`＝空文字に倒れ、GUI 素通しで無文言・無 CTA の Empty 面になりうる。must（名前付き3分岐 NoFolderOpened/SearchNoHits/AllConsumed の文言差分）は充足するため d_accept は割らないが、「行き止まりにしない」精神からは文言を 1 つ用意したい（frontend-ui・ux が独立に同一指摘）。
- **low** `view_state` の Error コメントが正典文言（WebView2 不在・衝突）を転記する一方、実装は `AccessDenied` のみを Error へ畳む（WebView2 不在=Partial、衝突=notification へ責務分離）。コメントと実態の乖離。
- **low** Loading の `total_count==0`（percent 不定）を spinner/確定で区別するフラグ不在。
- **low（security 備忘）** 診断ログの内容秘匿（要件 12.3）と画像ヘッダ width×height のオーバーフロー二重防御は、実 I/O 配線ターンで再確認（acceptance.md I11 に予約）。

> should の GUI 実体（画像簡易ビュー描画・診断ログ書き込み・About OSS 表示）は系統C に予約済みで、本フェーズでは ViewModel/ロジックと手動チェックリストまで。実描画・実書き込みの結線とコード署名なしの SmartScreen 手順検証は acceptance.md に集約。

## 停止理由と再開方法

- **status = done**（全 8 スプリント passed、上限未到達）。
- tamper: 全ターンで改竄なし（manifest フィルタ後の diff が空）。
- 中断回復について: 前セッションが sprint7 commit 後の step9 途中で中断し、working tree の `state.json` が巻き戻り・turn ctx が削除された状態で残っていた。本セッションは git コミットと各ターンの eval JSON（s6=94 / s7=97, ともに passed）を正として state を実状態へ整合（commit `1127176`）したうえで sprint8 を実行・通過させた。
- 再開が必要な場合: `/run-dev` 再実行で続きから（status=done のため通常は不要。post-loop の polish に着手する場合は持ち越し節を起点にする）。

## commit ハッシュ表（history）

| sprint.iter | score | passed | commit |
|---|---|---|---|
| 1.1 | 100 | ✅ | `e9ab567` |
| 2.1 | 100 | ✅ | `afba1d9` |
| 3.1 | 95 | ✅ | `0096801` |
| 4.1 | 90 | ✅ | `24e7d7b` |
| 5.1 | 86 | ❌(wip) | `63383e6` |
| 5.2 | 92 | ✅ | `cb33fd7` |
| 6.1 | 94 | ✅ | `d5ea507` |
| 7.1 | 97 | ✅ | `f5c074f` |
| 8.1 | 98 | ✅ | `feb6791` |

（中断回復コミット: `1127176` chore(dev): sprint7 step9 補完）
