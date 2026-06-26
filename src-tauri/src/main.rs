//! pika Tauri アプリ本体（windows subsystem の GUI）。
//!
//! 役割（design doc 3章）: TS frontend ⇔ Tauri command/event 境界 ⇔ pika-core。
//! 本ファイルは「薄い橋渡し層」に徹し、ロジックは pika-core に置く（逆参照禁止）。
//!
//! 本スプリント（sprint 1・最薄ループの土台）で立てるもの:
//! - WebView2 Runtime 不在時の最小ネイティブダイアログ経路（design doc 18章・要件2.3 改訂）
//! - フォルダを開く/保存の最小 command（中心体験① の貫通の土台）
//! - capability マップ（メイン=最小／プレビューWebView=ゼロ）の方針コメント

// 本番ビルドはコンソール窓を出さない GUI（design doc 9章 二段構成の GUI 側）。
// debug 時はパニックログを見たいので console を残す。
#![cfg_attr(all(not(debug_assertions), windows), windows_subsystem = "windows")]

mod access;
mod asset;
mod commands;
mod diagnostic;
mod document;
mod jumplist;
mod preview;
mod settings_service;
mod single_instance;
mod snapshot;
mod snapshot_persist;
mod state_store;
mod watcher;
mod webview2;

fn main() {
    // WebView2 Runtime 不在/破損時は Tauri 起動前に最小ネイティブダイアログで導入案内して終了する
    // （全Web化により WebView 無しでは UI を一切描けない＝design doc 18章・要件2.3 改訂）。
    if let Err(message) = webview2::ensure_runtime_available() {
        webview2::show_missing_runtime_dialog(&message);
        std::process::exit(1);
    }

    run();
}

/// Tauri アプリのビルドと起動。
///
/// capability マップ（design doc 9章）:
/// - メインウィンドウ（label "main"）: `capabilities/main.json` の最小集合のみ。
/// - プレビュー別WebView（setup で生成・label "preview"・[`preview::create_preview_webview`]）:
///   **capability ファイルを置かない**＝Tauri は未宣言ウィンドウへ plugin/core command を許可しない。
///
/// 【最重要セキュリティ境界・自前 app command の到達ガード】
/// Tauri v2 の ACL（capability）は plugin/core command をゲートするが、`generate_handler!` に登録した
/// **自前 app command は既定で全 WebView から呼べる**（メイン窓も custom-protocol オリジンの別WebView も
/// 区別しない。`pika-preview.localhost` はアプリ登録スキームのため "remote 扱いで自動拒否" も成立しない）。
/// 実機プローブで、プレビュー別WebView から `__TAURI_INTERNALS__.invoke('read_file', …)` が成功し
/// 任意ファイル読取に到達した（CVE-2024-35222 級の漏洩）。capability では塞げないため、ここで
/// **発信元 WebView ラベルを command ディスパッチ前に検査し、プレビューからの invoke を一律 reject** する
/// （唯一の堅牢な choke point。未信頼文書由来の JS が万一走っても command に到達させない＝design doc 6章/9章
/// 「Tauri API 到達不能」。多層防御の最後の砦）。
fn run() {
    // `generate_handler!` は引数なしの `move |invoke| { … }`（`Fn(Invoke<R>) -> bool`）へ展開される。
    // 変数束縛では `R` を推論できない（E0282）ため、束縛の型を `Invoke<tauri::Wry>` を取る関数として明示し
    // ランタイムを Wry に固定する。これを前段ガードで包み、プレビューWebView からの IPC は内側ハンドラ
    // （command ディスパッチ）へ渡さない。
    let app_invoke: fn(tauri::ipc::Invoke<tauri::Wry>) -> bool = tauri::generate_handler![
        commands::open_workspace,
        commands::list_dir,
        commands::read_file,
        commands::f5_resync,
        commands::save_app_state,
        commands::restore_app_state,
        commands::hash_content,
        commands::note_recent,
        commands::path_kind,
        commands::open_in_default_app,
        commands::open_log_folder,
        commands::create_entry,
        commands::delete_entry,
        snapshot::compute_file_diff,
        snapshot::confirm_file,
        snapshot::confirm_all,
        snapshot::rollback_file,
        preview::prepare_preview,
        preview::show_preview,
        preview::hide_preview,
        preview::set_preview_bounds,
        document::open_document,
        document::reopen_document_with_encoding,
        document::save_document,
        document::read_range,
        document::search_in_text,
        document::replace_in_text,
        document::replace_one,
        diagnostic::log_folder_path,
        settings_service::get_settings,
        asset::image_info,
    ];

    let app = tauri::Builder::default()
        // opener plugin（「ブラウザで開く」「ログフォルダを開く」= OS 既定アプリ起動・要件6.2/design G）。
        // 【最小権限】plugin を登録しても **その command ACL は capability で一切開放しない**
        // （capabilities/main.json は core:default のみ・opener permission 不付与＝frontend から
        // plugin command を直接 invoke できない）。利用は commands::open_in_default_app /
        // open_log_folder（自前の薄い command）の内部から opener の Rust API を呼ぶ経路に限定する。
        // プレビュー別WebView（権限ゼロ）へは当然付与しない（capability ファイル不在＝ゼロのまま）。
        .plugin(tauri_plugin_opener::init())
        // dialog plugin（OS ネイティブ選択ダイアログ・「フォルダを開く」「ファイルを開く」= 要件3.2/11.2）。
        // 【最小権限】capabilities/main.json は `dialog:allow-open` のみ付与（保存 `dialog:allow-save` は不付与）。
        // ダイアログで選んだパスは frontend から既存の open_workspace / read_file へ渡し、
        // AccessControl（set_root/verify_read）で core 再検証する（直接 FS read の近道は作らない）。
        // プレビュー別WebView（権限ゼロ）へは当然付与しない（capability ファイル不在＝ゼロのまま）。
        .plugin(tauri_plugin_dialog::init())
        // プレビュー custom protocol（pika-preview://）= Rust から別WebView へサニタイズ済み HTML を
        // 直配信する（HTML を JS のメインワールドに通さない＝design doc 6章）。CSP はレスポンスヘッダで強制。
        // この protocol が読むのは PreviewService（サニタイズ済み素材）のみで、Tauri command には到達しない。
        .register_uri_scheme_protocol(preview::PREVIEW_SCHEME, preview::handle_preview_request)
        // 画像配信 custom protocol（pika-asset://）= メインWebView 用の信頼画像配信（要件12.2・U3）。
        // アプリ全体登録だが、隔離の関門は origin に依らず (a) AccessControl の path ゲート
        // (b) is_sensitive 再判定 (c) プレビュー別WebView の CSP に pika-asset を入れない、の三重
        // （asset.rs のドキュメント参照）。ハンドラは verify_read 成功した実体パスのみを配信する。
        .register_uri_scheme_protocol(asset::ASSET_SCHEME, asset::handle_asset_request)
        .invoke_handler(move |invoke| {
            // 権限ゼロ別WebView（プレビュー・label = preview::PREVIEW_WEBVIEW_LABEL）からの IPC は
            // command 実行前に全拒否する（自前 app command は ACL で自動ゲートされない＝上記ドキュメント）。
            // メイン窓（label "main"）からの invoke はここを素通りし従来どおり全 command が通る。
            if preview::is_blocked_invoke_origin(invoke.message.webview_ref().label()) {
                invoke
                    .resolver
                    .reject("プレビューWebViewからのIPCは許可されていません");
                return true; // 処理済み（内側の command ディスパッチへ渡さない）。
            }
            app_invoke(invoke)
        })
        .setup(|app| {
            use tauri::Manager;

            // 単一インスタンス（design doc 15章-9）。CreateNamedPipe の成否を原子的ロックとし、
            // **ウィンドウ表示前に**サーバー公開（or 既存サーバーへの転送）を完了する。
            // クライアント（既に起動済み）なら引数を転送して即終了する（呼出プロセスは終了コード0）。
            let forward: Vec<String> = std::env::args().skip(1).collect();
            if let pika_core::ipc::InstanceRole::Client =
                single_instance::acquire_or_forward(app.handle(), &forward)
            {
                // 既存インスタンスへ転送済み。この後発プロセスは即終了する。
                app.handle().exit(0);
                return Ok(());
            }

            // 監視サービスを managed state として登録（command から State で取得する）。
            // 監視スレッドはここでは起動せず、open_workspace でルート確定時に開始する
            // （フォルダ未オープン時は監視コストゼロ＝軽い）。
            app.manage(watcher::WatcherService::new(app.handle().clone()));
            // スナップショット/差分/確認済みサービス（ベースライン索引＋内容 object）を登録する。
            app.manage(snapshot::SnapshotService::new());
            // パスアクセス制御（任意ファイル読み書き封じ込め・#5/#46）。open_workspace で root を張り、
            // open-request / restore タブで個別ファイルを許可する。I/O command はこのゲートを通す。
            app.manage(access::AccessControl::new());
            // プレビューサービス（サニタイズ済みレスポンスを世代キーで保持・custom protocol が引く）。
            app.manage(preview::PreviewService::new());
            // 検索/置換のキャンセルトークン置き場（新しい検索で前のを打ち切る＝固まらない・要件5.4）。
            app.manage(document::SearchCancelService::new());
            // 設定サービス（settings.toml の監視反映・要件10.3/10.4）。data_root を state.json と同じ
            // 手段（state_store::resolve）で解決し `<root>/settings.toml` を見る。起動時に一度ロードして
            // current を確定し（破損なら既定＋警告）、managed state（Arc 共有）として登録する。
            // ポーリング監視スレッドはイベントループ稼働後（RunEvent::Ready）に起動して起動を遅延ブロックしない。
            // resolve 失敗（%LOCALAPPDATA% 不在等）でも編集体験は継続させる（設定が既定のまま動く）。
            match state_store::resolve() {
                Ok(root) => {
                    let path = settings_service::settings_path(&root.path);
                    let service = std::sync::Arc::new(settings_service::SettingsService::new(path));
                    service.load_initial(&app.handle().clone());
                    app.manage(service);
                }
                Err(e) => {
                    eprintln!("設定サービスのデータルート解決に失敗しました（既定設定で継続）: {e}");
                    // 解決できなくても get_settings が動くよう、既定値の managed state を置く
                    // （path は使われない＝監視スレッドは起動しない）。
                    let service = std::sync::Arc::new(settings_service::SettingsService::new(
                        std::path::PathBuf::new(),
                    ));
                    app.manage(service);
                }
            }
            // プレビュー別WebView（label "preview"・権限ゼロ）はここでは生成しない。
            // Windows/WebView2 では子WebView コントローラの生成完了はメッセージループ経由で通知されるため、
            // イベントループ未稼働の setup 内で add_child の完了を待つとデッドロックする（メイン窓が不可視のまま固着）。
            // 生成はループ稼働後の RunEvent::Ready へ遅延させる（下記 app.run のコールバック）。
            if let Some(win) = app.get_webview_window("main") {
                let _ = win.show();
            }
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("pika の起動に失敗しました");

    // イベントループを起動し、稼働後（RunEvent::Ready）に子WebView を生成する。
    // RunEvent::Ready のコールバックはメインスレッドでループ稼働中に発火するため、
    // add_child 内部のメッセージポンプが回り、setup 内で起きていたデッドロックを回避できる。
    app.run(|app_handle, event| {
        if let tauri::RunEvent::Ready = event {
            // プレビュー別WebView（label "preview"・権限ゼロ）をメイン窓へオーバーレイ生成する
            // （design doc 6章/9章）。capability ファイルに preview を含めないことで Tauri API 到達不能を保つ。
            // 初期は hidden・極小サイズ・about:blank。frontend が show_preview で矩形配置・ナビゲートする。
            if let Err(e) = preview::create_preview_webview(app_handle) {
                // 生成失敗でも編集体験は継続させる（プレビューのみ不能・最上位「固まらない/データを失わない」）。
                eprintln!("プレビュー別WebView の生成に失敗しました: {e}");
            }
            // settings.toml のポーリング監視を起動する（要件10.3 再起動なし反映／10.4 不完全保存の直前維持）。
            // 起動を遅延ブロックしないようイベントループ稼働後のここで始める（最初のポーリングは間隔後）。
            use tauri::Manager;
            let service =
                app_handle.state::<std::sync::Arc<settings_service::SettingsService>>();
            service.spawn_watch(app_handle.clone());
        }
    });
}
