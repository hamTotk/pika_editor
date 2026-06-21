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

mod commands;
mod diagnostic;
mod document;
mod jumplist;
mod preview;
mod single_instance;
mod snapshot;
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
///   **capability ファイルを置かない**＝Tauri は未宣言ウィンドウへ command を一切許可しない（権限ゼロ）。
///   さらに別WebView は remote オリジン（pika-preview.localhost）へナビゲートするため、Tauri の ACL は
///   remote からの app command を拒否する。これにより未信頼文書 WebView から `invoke`/`__TAURI_INTERNALS__`
///   経由の任意 command が到達不能になる（design doc 6章/15章-3・到達不能の実証は系統C）。
fn run() {
    let app = tauri::Builder::default()
        // プレビュー custom protocol（pika-preview://）= Rust から別WebView へサニタイズ済み HTML を
        // 直配信する（HTML を JS のメインワールドに通さない＝design doc 6章）。CSP はレスポンスヘッダで強制。
        // この protocol が読むのは PreviewService（サニタイズ済み素材）のみで、Tauri command には到達しない。
        .register_uri_scheme_protocol(preview::PREVIEW_SCHEME, preview::handle_preview_request)
        .invoke_handler(tauri::generate_handler![
            commands::open_workspace,
            commands::read_file,
            commands::save_file,
            commands::f5_resync,
            commands::save_app_state,
            commands::restore_app_state,
            commands::hash_content,
            commands::note_recent,
            commands::path_kind,
            snapshot::compute_file_diff,
            snapshot::confirm_file,
            snapshot::confirm_all,
            snapshot::rollback_file,
            preview::prepare_preview,
            preview::show_preview,
            preview::hide_preview,
            preview::set_preview_bounds,
            document::open_document,
            document::save_document,
            document::read_range,
            document::search_in_text,
            document::replace_in_text,
            diagnostic::log_folder_path,
        ])
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
            // プレビューサービス（サニタイズ済みレスポンスを世代キーで保持・custom protocol が引く）。
            app.manage(preview::PreviewService::new());
            // 検索/置換のキャンセルトークン置き場（新しい検索で前のを打ち切る＝固まらない・要件5.4）。
            app.manage(document::SearchCancelService::new());
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
        }
    });
}
