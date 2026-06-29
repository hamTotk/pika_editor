; pika NSIS インストーラーフック（要件13・design doc sprint7）
; ------------------------------------------------------------------------------
; Tauri v2 の NSIS テンプレートが各段で呼ぶマクロを定義する。
; tauri.conf.json の bundle.windows.nsis.installerHooks から読み込まれる。
;
; 担うこと:
;   1. エクスプローラー統合の登録/解除（要件3.3）。ロジックは pika-core::explorer に集約され、
;      同梱した pika-cli.exe（externalBin・INSTDIR 配置）の --register-shell / --unregister-shell
;      を呼ぶだけにする（単一ソース。NSIS 側でレジストリ値を二重定義しない）。
;   2. アンインストール時のユーザーデータ処理（要件13）。設定・状態・ログは削除し、
;      退避スナップショット（最後の砦になりうるバックアップ）は「残す（既定）/消す」を選ばせる。
;
; インストール版のデータルートは %LOCALAPPDATA%\pika\（pika-core::data_root。
; settings.toml/state.json/snapshots/logs を一元配置。要件文の %APPDATA% 記述は doc 不整合で
; 実装は data_root 一貫＝settings.rs の注記どおり）。ポータブル版は登録もデータ削除も行わない
; （installer 経由でないため本フックは実行されない）。

; ── インストール完了後: エクスプローラー統合を登録 ──────────────────────────────
!macro NSIS_HOOK_POSTINSTALL
  ; HKCU に候補（OpenWithProgids）と右クリック「pikaで開く」を登録する。
  ; 失敗してもインストール自体は成功扱いにする（関連付けは任意機能・後から手動登録も可能）。
  ; nsExec はウィンドウを出さずに実行する（pika-cli は console subsystem）。
  ;
  ; 【installMode=currentUser 固定】登録先は HKCU（pika-core::explorer。全ユーザー＝HKLM 未対応）。
  ; インストーラは「現在のユーザーのみ」固定（perMachine は提供しない）。pika は per-user 設計
  ; （登録先 HKCU・データルート %LOCALAPPDATA%）であり、昇格しないので登録先 HKCU は実際に pika を
  ; 使うユーザーと一致する。全ユーザー導入が要るなら各ユーザーが自分でインストールする。
  nsExec::Exec '"$INSTDIR\pika-cli.exe" --register-shell'
!macroend

; ── アンインストール開始前: エクスプローラー統合を解除 ──────────────────────────
!macro NSIS_HOOK_PREUNINSTALL
  ; ファイル削除より前に解除する（pika-cli.exe がまだ INSTDIR に存在するうちに呼ぶ）。
  ; 候補・右クリック・残った空キーを掃除し、関連付けの残骸を残さない（要件13）。
  nsExec::Exec '"$INSTDIR\pika-cli.exe" --unregister-shell'
!macroend

; ── アンインストール完了後: ユーザーデータを処理 ────────────────────────────────
!macro NSIS_HOOK_POSTUNINSTALL
  ; 設定・状態・ログは常に削除する（要件13）。
  ; データルート直下の永続ファイルは settings.toml と state.json のみ（最近使った項目は state.json 内）。
  ; index.json/meta.json/objects は snapshots\ 配下＝退避データの一部なので、ここでは触らない。
  ; installMode=currentUser 固定（昇格しない）ため $LOCALAPPDATA は実際に pika を使ったユーザーを指す
  ; ＝削除もスナップショット確認も正しいプロファイルへ届く（perMachine だと昇格アカウントを掃除して
  ; しまい実ユーザーのデータが残置する問題があり、currentUser 固定で回避している）。
  Delete "$LOCALAPPDATA\pika\settings.toml"
  Delete "$LOCALAPPDATA\pika\state.json"
  RMDir /r "$LOCALAPPDATA\pika\logs"

  ; 退避スナップショットはユーザー選択（既定＝残す）。
  ; サイレント（/S）時は /SD IDYES で「残す」に倒す（データ保全を優先）。
  MessageBox MB_YESNO|MB_ICONQUESTION "退避スナップショット（Pika Editor が保持するバックアップ。最後の砦になりうる）を残しますか？$\n$\n［はい］残す（既定）  ／  ［いいえ］完全に削除する" /SD IDYES IDYES pika_keep_snapshots
    RMDir /r "$LOCALAPPDATA\pika\snapshots"
  pika_keep_snapshots:

  ; pika フォルダ自体は、中身が空（＝snapshots も消した）のときだけ削除する。
  ; snapshots を残した場合はフォルダごと残る（RMDir は非空なら何もしない）。
  RMDir "$LOCALAPPDATA\pika"
!macroend
