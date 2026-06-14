# GUI 本体（pika）と コンソールスタブ（pika.com）。
# PIKA_BUILD_GUI=ON のときのみ構成される（wxWidgets/WebView2 を含む重量パス）。
#
# sprint 1 では「ターゲットが design.md 12章どおり分割定義されている」ことを満たすための
# 最小スタブ。中身（CLI -g パース・シングルインスタンス・ウィンドウ生成）は後続スプリント。

find_package(wxWidgets CONFIG REQUIRED)

# ---- pika: GUI exe（WIN32 サブシステム）----
add_executable(pika WIN32
    main_gui.cpp
)
target_link_libraries(pika
    PRIVATE
        pika_warnings
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
