# GUI 本体（pika）と コンソールスタブ（pika.com）。
# PIKA_BUILD_GUI=ON のときのみ構成される（wxWidgets/WebView2 を含む重量パス）。
#
# sprint3（中心体験①『開く』の GUI/プラットフォーム配線）:
# - main_gui を実体化（データルート/CLI 受領→単一インスタンス判定→サーバー公開→MainFrame 表示→列挙）
# - pipe_server: CreateNamedPipe の原子的ロック・敗者クライアント転送（実 I/O。系統B/C）
# - dir_enumerator: 実 FS の逐次列挙（controller::normalize_entries へ供給）
# - src/ui: MainFrame・FileTreePanel（wxDataViewCtrl）・EditorPanel（wxStyledTextCtrl）・ui_messages
# 判断ロジックは controller/core（wx 非依存・gtest 済み）に置き、ここは実配線に徹する。

find_package(wxWidgets CONFIG REQUIRED)

# ---- pika_ui: wx 依存の UI 層（MainFrame・ツリー・エディタ・文言）----
# main_gui（app）と将来のテスト/別 exe から再利用できるよう静的lib に切り出す。
add_library(pika_ui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/editor_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/editor_panel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/file_tree_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/file_tree_panel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/main_frame.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ui_messages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ui_messages.h
    # プラットフォーム層（実 FS/OS I/O。MainFrame が直接呼ぶ列挙）。
    dir_enumerator.cpp
    dir_enumerator.h
)
target_link_libraries(pika_ui
    PUBLIC
        pika_core
        wx::core
        wx::base
        wx::aui
        wx::stc
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
