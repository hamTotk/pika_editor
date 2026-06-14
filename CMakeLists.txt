# pika ルート CMakeLists.txt（vcpkg manifest モード）
#
# ターゲット分割（design.md 12章）:
#   pika_core  … 静的ライブラリ。wx 非依存部（core/ ＋ util）。exe とテスト両方からリンクする
#   pika       … GUI exe（wxWidgets）。PIKA_BUILD_GUI=ON のときのみ
#   pika.com   … コンソールサブシステムの薄いスタブ（--help/--version・引数検証・終了コード）
#   pika_tests … gtest。PIKA_BUILD_TESTS=ON のときのみ
#
# core-test プリセット（x64-core-test）は wx/WebView2 を含まず pika_core + pika_tests のみを構成する。
cmake_minimum_required(VERSION 3.21)

project(pika LANGUAGES CXX VERSION 0.0.1)

option(PIKA_BUILD_GUI "wxWidgets を含む GUI 本体（pika exe）をビルドする" ON)
option(PIKA_BUILD_TESTS "gtest（pika_tests）をビルドする" ON)

# ---- 全体方針 ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静的 CRT（/MT）。triplet（x64-windows-static）と一致させ、依存も静的リンクする
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# IDE 用の分類
if(MSVC)
    add_compile_options(/utf-8)
endif()

# pika 自前ターゲットにのみ適用する警告設定（/W4・警告エラー扱い）。
# 外部（vcpkg）ヘッダには適用しない（design.md 12章「外部ヘッダ除く」）。
# INTERFACE ライブラリにまとめ、自前ターゲットだけが link する。
add_library(pika_warnings INTERFACE)
if(MSVC)
    target_compile_options(pika_warnings INTERFACE /W4 /WX /permissive-)
else()
    target_compile_options(pika_warnings INTERFACE -Wall -Wextra -Werror)
endif()

# Release の最適化（/O2・LTCG）。VS マルチコンフィグでは Release 構成にのみ効かせる。
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg LANGUAGES CXX)
if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# ---- pika_core（wx 非依存：core/ ＋ util）----
add_subdirectory(src)

# ---- テスト ----
if(PIKA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
