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
/// - プレビュー別WebView（sprint 4 で生成・label "preview"）: **capability ファイルを置かない**＝
///   Tauri は未宣言ウィンドウへ command を一切許可しない（権限ゼロ）。これにより未信頼文書 WebView から
///   `invoke`/`__TAURI_INTERNALS__` 経由の任意 command が到達不能になる（design doc 6章/15章-3）。
fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            commands::open_workspace,
            commands::read_file,
            commands::save_file,
            commands::f5_resync,
        ])
        .setup(|app| {
            // 起動時にメインウィンドウを表示（visible:false で生成し、初期化後に出す）。
            use tauri::Manager;
            // 監視サービスを managed state として登録（command から State で取得する）。
            // 監視スレッドはここでは起動せず、open_workspace でルート確定時に開始する
            // （フォルダ未オープン時は監視コストゼロ＝軽い）。
            app.manage(watcher::WatcherService::new(app.handle().clone()));
            if let Some(win) = app.get_webview_window("main") {
                let _ = win.show();
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("pika の起動に失敗しました");
}
