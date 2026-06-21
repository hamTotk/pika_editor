# turn-5-1 generator report — sprint 5（CLI 二段構成・単一インスタンス・状態復元）

sprint goal: pika-cli.exe（console・--help/--version/引数検証/終了コード）＋pika.exe（GUI）の二段構成、
自前 named pipe による単一インスタンス＋引数転送（受信引数は core 検証層で再正規化）、-g パース、
state.json アトミック書込＋復元3分岐、最近使った項目＋ジャンプリストを結線する（design doc 15章-9）。

決定論ロジックは pika-core（cargo test 済み）へ、OS 呼び出し（named pipe / spawn / アトミック書込）は
pika-cli / src-tauri（compile/型ゲート）へ分離した。

## 変更ファイル一覧

### pika-core（決定論ゲート対象・新規/拡張）
- `crates/pika-core/src/path_verify.rs`（新規）— 受信パスの再正規化・再検証（相対拒否/NUL・制御文字拒否/ADS 拒否/
  UNC・ドライブ絶対・拡張長パス分類/長パスへ `\\?\`・`\\?\UNC\` 接頭辞付与）。tests 12 件。
- `crates/pika-core/src/ipc.rs`（新規）— 単一インスタンス named pipe プロトコルの純粋ロジック（役割決定 decide_role・
  パイプ名組立 build_pipe_name〔SID 注入検査〕・転送 JSON 組立 build_forward_message・受信検証 parse_incoming_message
  〔≤8KB 打切り・JSON スキーマ・version 安全側・パス再検証・受理操作=パスオープン限定〕）。tests 14 件。
- `crates/pika-core/src/state.rs`（新規）— AppState の serde 往復・load_state（version 安全側＝UnknownVersion/Corrupt/Ok）・
  restore_tab（復元3分岐＝消失/別物/一致）・restore_workspace（消失=空状態へ安全遷移）。tests 14 件。
- `crates/pika-core/src/cli.rs`（拡張）— normalize_to_absolute（相対→cwd 前置の絶対化・ドライブ相対は安全側で拒否）。tests +6 件。
- `crates/pika-core/src/lib.rs`（拡張）— ipc / path_verify / state モジュール登録。
- `crates/pika-core/Cargo.toml`・`Cargo.toml`（root）— serde / serde_json を workspace.dependencies に追加。

### CLI（console subsystem・二段構成の前段）
- `crates/pika-cli/src/main.rs`（書換）— 引数検証→絶対パス正規化（core 委譲）→pika.exe spawn。終了コード 0=受理/2=引数
  エラー/3=GUI 起動失敗。-g は parse_goto_spec→normalize_to_absolute→rebuild_goto_spec で組み直し。tests 11 件。

### src-tauri（薄い橋渡し層・compile/型ゲート）
- `src-tauri/src/single_instance.rs`（新規）— Windows named pipe 配線（CreateNamedPipeW の成否＝原子的ロック・
  SDDL owner/System 限定 DACL・PIPE_REJECT_REMOTE_CLIENTS・受信は core 検証・emit('open-request')＋前面化・
  クライアント転送→exit(0)）。純粋ロジックは全て pika-core::ipc / path_verify に委譲。
- `src-tauri/src/state_store.rs`（新規）— データルート解決＋state.json アトミック書込（一時ファイル→rename）＋
  FS から PathProbe を作り core の復元判定を呼ぶ。未知バージョン/破損は safe_empty で空起動＋上書き禁止。
- `src-tauri/src/commands.rs`（拡張）— save_app_state / restore_app_state command（DTO 変換のみ・判定は core）。
- `src-tauri/src/main.rs`（拡張）— setup でウィンドウ表示前に single_instance::acquire_or_forward を実行し、
  Client なら exit(0)。command を 2 件登録。
- `src-tauri/Cargo.toml`（拡張）— windows-sys に Win32_System_Pipes/IO/Security/Authorization/Threading 等を追加。

### frontend（型ゲート）
- `src/ipc.ts`（拡張）— AppState/TabState/RestoreOutcome/OpenRequestPayload 型・saveAppState/restoreAppState/onOpenRequest。
- `src/main.ts`（拡張）— 起動時 restoreOnStartup（ワークスペース復元/タブ3分岐/safe_empty 保存抑止）・
  persistAppState（beforeunload/フォルダ開く/タブ開く）・onOpenRequest（転送パスを開く）。
- `src/editor/index.ts`（拡張）— EditorHandle に gotoPosition を追加（-g 行・桁へカーソル・行超過=最終行/桁超過=行末クランプ）。

### docs（系統C 所見台帳・書込可）
- `docs/acceptance-findings.md`（拡張）— T-008（CLI 二段構成・単一インスタンス・状態復元）を追加。決定論側/配線/
  系統C で残す実機確認（H1〜H7 を新スタック再検証扱い）/状態を記録。

## must criteria 実装状況

1. **CLI 二段構成**: 実装。pika-cli が --help/--version/引数検証/終了コードを同期処理し、core で絶対パス正規化して
   pika.exe を spawn。終了コード規約 0/2/3。`pika --version` は素の文字列のみ（リダイレクト/文字化け対策）。
   → 自動テスト: pika-cli tests（help/version/未知オプション/-g 検証/転送引数組立）。実機の対話シェル復帰/リダイレクト
   取得は系統C（T-008・acceptance H5）。
2. **-g パース（pika-core）**: 既存 parse_goto_spec（ドライブレターのコロン非分割・桁省略=行頭・非整数=位置無視・
   行数超過=最終行はフロント gotoPosition でクランプ）＋ normalize_to_absolute（相対→絶対）を追加。
   → 自動テスト: cli tests（既存 11＋新規 6）。
3. **自前 named pipe（単一インスタンス）**: 実装。`\\.\pipe\pika-<SID>`・owner/System 限定 DACL（SDDL）・
   PIPE_REJECT_REMOTE_CLIENTS・受信≤8KB打切り・JSON スキーマ検証・受理操作=パスオープン限定・CreateNamedPipe の
   成否を原子的ロック・獲得失敗はクライアントとして絶対パス正規化済み引数を転送し終了コード0・サーバー公開は
   ウィンドウ表示前に完了。→ 自動テスト: ipc tests（役割決定/パイプ名組立/SID 注入拒否/転送 JSON 往復/打切り/
   非JSON拒否/未知 version 拒否/受信パス再検証/余分フィールド無視＝受理操作限定）。DACL・リモート拒否の実効と
   即終了/前面化は系統C（T-008・H1/H4）。
4. **受信引数の再検証（pika-core）**: 実装。path_verify が転送パスを信頼せず再正規化・再検証（絶対パス化・
   UNC/ADS/長パス接頭辞の扱い確定・NUL/制御文字の健全性検査）。parse_incoming_message が各パスを path_verify へ通す。
   → 自動テスト: path_verify tests 12＋ipc の「受信パス再検証/ADS 拒否」。
5. **state.json 復元3分岐（pika-core）**: 実装。AppState（フォルダ/タブ/カーソル/スクロール/表示モード/差分トグル/
   ツリー展開/ウィンドウ状態）を to_json で直列化（アトミック書込は state_store）・load_state で version 安全側
   （未知=読まず/書かず/再生成せず・破損=空起動）・restore_tab で3分岐（消失=削除済み表示/別物=未読復元/
   ワークスペース消失=空状態へ安全遷移）。→ 自動テスト: state tests 14。
6. **cargo test PASS / cargo build / frontend 型チェック exit 0**: 全て成立（下記「自己実行した verify」）。
7. **design doc 15章-9 単一インスタンス実機（系統C）**: T-008 に記録（H1=即終了・前面化、H2=-g 行ジャンプ、
   H4=キャンセルでも終了コード0、を Windows 実機 Release で確認する旨）。これは GUI 実機が要るため自動 verify に載せない。

## should criteria 実装状況

- **最近使った項目＋ジャンプリスト（要件10.2）**: 未実装（テスト化できず・下記参照）。AppState は recent を持たず、
  ジャンプリストは windows crate COM（ICustomDestinationList）が要り実機検証主体のため本ターンでは見送り。turn report に
  明記して評価へ委ねる。
- **フォルダ切替（要件3.2）**: 部分。onOpenRequest で転送パスを開く配線は入れたが、起動中の別フォルダ指定による
  フォルダ切替＋未保存確認は frontend で未配線（タブ確認フローは sprint 3 の withBusy 系に集約済み・本ターンは触らず）。
- **存在しないファイルパス=新規タブ/存在しないフォルダ=エラー（要件3.2）**: 未配線（open_workspace は is_dir 検査で
  フォルダ不在をエラーにするのみ。新規タブ化は sprint 6 の document/保存系で扱う想定）。

## テスト化できなかった criteria とその理由

- **must 7（単一インスタンス実機・H1/H2/H4）**: GUI 実機（既存プロセスへの転送・前面化・終了コード観測）が要るため
  cargo test 不能。系統C（T-008）へ記録。named pipe の DACL/リモート拒否の実効も同様（別ユーザー/リモート接続は実機）。
- **CLI の対話シェルプロンプト復帰/リダイレクト出力エンコーディング**: console subsystem の端末挙動は実機確認（H5）。
  終了コードと引数検証は pika-cli tests で観測。
- **named pipe / spawn / アトミック書込の OS 呼び出し**: FFI 実行は単体テスト不能。純粋ロジック（役割決定・パイプ名・
  転送 JSON・スキーマ検証・パス再検証・state 直列化/復元3分岐）を pika-core へ切り出し全て cargo test で観測。
  OS 呼び出し層は cargo build（警告エラー扱い）＋型チェックで最低保証（spec.md 系統A補）。
- **should（ジャンプリスト）**: ICustomDestinationList は COM＋実機表示主体で決定論テストに向かないため見送り。

## 自己実行した verify の結果（sprint verify: cargo test / cargo build）

- `cargo test`: PASS。pika-core 193 件（本スプリント新規=cli 6・ipc 14・path_verify 12・state 14）／pika-cli 11 件／
  pika-app(bin) 5 件。全 0 failed。
- `cargo build`（crates＋src-tauri・workspace lints warnings=deny）: Finished・exit 0（警告ゼロ）。
- `npm run typecheck`（tsc -p tsconfig.app.json）: exit 0（エラーゼロ）。

合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

DONE: C:/dev/pika_editor/dev/turns/turn-5-1-generator.md
