# GUI 本体（pika）と コンソールスタブ（pika.com）。
# PIKA_BUILD_GUI=ON のときのみ構成される（wxWidgets/WebView2 を含む重量パス）。
#
# sprint3（中心体験①『開く』の GUI/プラットフォーム配線）:
# - main_gui を実体化（データルート/CLI 受領→単一インスタンス判定→サーバー公開→MainFrame 表示→列挙）
# - pipe_server: CreateNamedPipe の原子的ロック・敗者クライアント転送（実 I/O。系統B/C）
# - dir_enumerator: 実 FS の逐次列挙（controller::normalize_entries へ供給）
# - src/ui: MainFrame・FileTreePanel（wxDataViewCtrl）・EditorPanel（wxStyledTextCtrl）・ui_messages
# 判断ロジックは controller/core（wx 非依存・gtest 済み）に置き、ここは実配線に徹する。
# sprint5（中心体験③『差分』の WebView2 配線）:
# - src/ui/preview_view: 共有 1 枚 wxWebView（Edge）の遅延生成・仮想ホスト doc.pika のカスタム
#   リソースハンドラ（親フォルダ配下のみ・../遮断）・ナビゲーション全インターセプト・JS 有効無効の
#   直列切替の実配線。HTML 組み立て（CSP/base/色非依存差分/grid）は controller::preview_builder
#   （wx 非依存・gtest 済み）が担い、ここは「どの HTML をいつナビゲートし占有世代を照合するか」に徹する。

find_package(wxWidgets CONFIG REQUIRED)
# WebView2 ローダ（静的）。wxWidgets[webview] の Edge バックエンドが要求する
# WebView2LoaderStatic.lib をフルパスで解決する（wx の config は bare 名で参照するため、
# unofficial::webview2::webview2 をリンクしてリンカ検索パスに載せる）。
find_package(unofficial-webview2 CONFIG REQUIRED)

# ---- pika_ui: wx 依存の UI 層（MainFrame・ツリー・エディタ・文言）----
# main_gui（app）と将来のテスト/別 exe から再利用できるよう静的lib に切り出す。
add_library(pika_ui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/editor_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/editor_panel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/file_tree_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/file_tree_panel.h
    # F-022: 画像簡易ビュー（wxImage 自前描画・WebView2 不使用）と非対応/巨大画像ビュー（要件12.2）。
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/image_view_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/image_view_panel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main_frame.h
    # sprint5: 共有 1 枚 WebView2 のプレビュー/差分ファサード（wx 依存・系統B）。
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/preview_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/preview_view.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ui_messages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ui_messages.h
    # F-022: 巨大画像/バイナリの非対応ビュー（中央ラベル＋「既定のアプリで開く」。要件12.2）。
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/unsupported_view_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/unsupported_view_panel.h
    # プラットフォーム層（実 FS/OS I/O。MainFrame が直接呼ぶ列挙・監視）。
    dir_enumerator.cpp
    dir_enumerator.h
    # sprint4: ReadDirectoryChangesW 監視スレッド実体（FILE_NOTIFY_INFORMATION→RawEvent→WatcherCore）。
    watch_thread.cpp
    watch_thread.h
    # F-026: 系統C A章（性能ゲート）の in-app 計測（QPC タイミング・プロセスツリーメモリ合算）。
    # Win32（QPC/psapi/Toolhelp32）はこのプラットフォーム層に閉じる（core/ には持ち込まない）。
    perf_log.cpp
    perf_log.h
)
target_link_libraries(pika_ui
    PUBLIC
        pika_core
        wx::core
        wx::base
        wx::aui
        wx::stc
        # sprint5: 共有 1 枚 WebView2（Edge バックエンド）でプレビュー/差分を描画する（design 6章）。
        wx::webview
        # WebView2 ローダ（静的）をフルパスで供給する（wx の bare 名参照のリンク不全を塞ぐ）。
        unofficial::webview2::webview2
        # F-026: 性能計測のメモリ取得（GetProcessMemoryInfo）。psapi は Windows SDK 同梱（依存追加なし）。
        psapi
    PRIVATE
        pika_warnings
)

# ---- pika: GUI exe（WIN32 サブシステム）----
add_executable(pika WIN32
    main_gui.cpp
    pipe_server.cpp
    pipe_server.h
)
target_link_libraries(pika
    PRIVATE
        pika_warnings
        pika_ui
        pika_core
        wx::core
        wx::base
        # クラッシュ診断（未処理例外フィルタ）のシンボル解決に DbgHelp を使う（main_gui.cpp）。
        dbghelp
)

# クラッシュ時にシンボル名・行番号を出すため Release でも PDB を生成する（要件12.3 の診断ログ）。
# 最適化（/O2・LTCG）は維持したまま /Zi でコンパイル、/DEBUG でリンクして .pdb を残す。
# /OPT:REF /OPT:ICF を併用し /DEBUG による最適化抑制を打ち消す（コードは最適化済みのまま）。
if(MSVC)
    target_compile_options(pika PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(pika PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

# 同梱アセット（preview.css 等）を exe 隣の assets/ へ配置する（app.pika 仮想ホストの配信元。
# design 6章 C6・sprint5）。MainFrame::asset_dir() が exe ディレクトリ＋"assets" を解決するため、
# ビルド後に同期しておく（系統C の WebView2 実描画でスタイルが当たる）。
add_custom_command(TARGET pika POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:pika>/assets"
    COMMENT "Copy bundled assets (preview.css) next to pika.exe for app.pika virtual host"
)

# ---- pika.com: コンソールサブシステムの薄いスタブ ----
# 名前を pika.com にして、エクスプローラー/シェルが pika より先に解決できるようにする
# （--help/--version・引数検証・終了コードをコンソールで返す。design.md 12章・要件3章）。
add_executable(pika_com
    main_console.cpp
)
target_link_libraries(pika_com
    PRIVATE
        pika_warnings
        pika_core
)
set_target_properties(pika_com PROPERTIES
    OUTPUT_NAME "pika"
    SUFFIX ".com"
)
