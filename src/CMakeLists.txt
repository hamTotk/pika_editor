# pika_core … wx 非依存の静的ライブラリ（core/ ＋ util）。exe とテスト両方からリンクする。
#
# sprint 2 では util の土台（Result<T>・エンコーディング判定/往復・LF正規化ハッシュ・
# アトミック書き込み・TaskRunner・Logger）を実装する。
# sprint 3 では core/watcher（イベント合成・自己保存抑制・rename正規化・確定読み・
# オーバーフロー再同期）を UI 非依存ロジックとして実装する。
# sprint 4 では core/render（ホワイトリストサニタイズ・JS依存検知・外部リソース検知・CSP組み立て・
# レンダリング暴走ガード判定・md4c GFM 変換）を WebView2 非依存ロジックとして実装する。
# sprint 5 では core/snapshot（ベースライン・退避の保存/復元・容量管理・index破損復元・mark-and-sweep・
# 機密ファイルのハッシュのみ記録）を UI 非依存ロジックとして実装する（内容 object は zstd 圧縮）。
# sprint 6 では core/diff（行差分=dtl/Myers・行内の単語/文字単位ハイライト・累積差分・LF正規化照合・
# 大規模入力ガード・色非依存 +/- 出力・協調キャンセル）を UI/WebView2 非依存ロジックとして実装する。
# sprint 7 では core/ipc（CLI -g パース・引数検証/分類・終了コード・相対パス正規化・IPC スキーマ検証・
# 名前付きパイプの命名/保護方針）を UI/Win32 非依存ロジックとして実装する。
# sprint 8 では core/settings（settings.toml の読み取り専用ロード・不正値フォールバック・直前値維持）と
# core/workspace（ツリーモデル＝フォルダ先行/自然順・除外リスト適用・未読集合と子孫伝播・rename引き継ぎ）を
# UI 非依存ロジックとして実装する。
# sprint 9 では core/search（PCRE2=pcre2-16/UTF。検索・正規表現置換・キャプチャ参照・Unicode文字クラス・
# 巨大入力ガード・協調キャンセル）を UI 非依存ロジックとして実装する。
# sprint 10 では core/document（退避フロー結合＝ReviewFlow。incoming/conflict/rollback/baseline-replace の
# 退避・確認済みのベースライン更新・巻き戻し非活性判定・すべて確認済みの一括取消）と core/state（state.json の
# version・アトミック書き込み・未知version安全側）を UI 非依存ロジックとして実装する。

find_package(xxHash CONFIG REQUIRED)
find_package(md4c CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(toml11 CONFIG REQUIRED)
# PCRE2（pcre2-16/UTF。core/search の内部だけで使う＝公開ヘッダに PCRE2 型を出さない）。
find_package(pcre2 CONFIG REQUIRED)

# dtl（Diff Template Library）はヘッダオンリーで CMake config を持たない（vcpkg は
# include/dtl/ にヘッダを配置するのみ）。find_path でヘッダ位置を解決し、core/diff だけに
# PRIVATE で渡す（実装詳細として漏らさない）。
find_path(DTL_INCLUDE_DIR NAMES dtl/dtl.hpp REQUIRED)

# controller … UI とコアの仲介ロジック（ViewModel・状態機械）の wx 非依存部。
# 系統A（決定論ゲート）の対象として pika_core に含め、PIKA_BUILD_GUI の影響下に置かない
# （spec.md 補完判断2）。wx 依存のビュー結線は src/ui 側（GUI スプリント）が担う。
# sprint1 では TreeViewModel（build_tree/UnreadSet → 状態マーク・種別アイコンへの写像）を実装する。
add_library(pika_core STATIC
    controller/tree_view_messages.cpp
    controller/tree_view_messages.h
    controller/tree_view_model.cpp
    controller/tree_view_model.h
    core/diff/cancel_token.h
    core/diff/diff_engine.cpp
    core/diff/diff_engine.h
    core/diff/diff_types.h
    core/diff/inline_diff.cpp
    core/diff/inline_diff.h
    core/diff/line_diff.cpp
    core/diff/line_diff.h
    core/document/review_flow.cpp
    core/document/review_flow.h
    core/state/state_io.cpp
    core/state/state_io.h
    core/state/state_types.h
    core/render/csp_builder.cpp
    core/render/csp_builder.h
    core/render/html_inspector.cpp
    core/render/html_inspector.h
    core/render/html_sanitizer.cpp
    core/render/html_sanitizer.h
    core/render/html_tokenizer.cpp
    core/render/html_tokenizer.h
    core/render/markdown_renderer.cpp
    core/render/markdown_renderer.h
    core/render/render_guard.cpp
    core/render/render_guard.h
    core/render/render_options.h
    core/render/url_classifier.cpp
    core/render/url_classifier.h
    core/ipc/cli_parser.cpp
    core/ipc/cli_parser.h
    core/ipc/ipc_message.cpp
    core/ipc/ipc_message.h
    core/ipc/path_normalizer.cpp
    core/ipc/path_normalizer.h
    core/ipc/pipe_security.cpp
    core/ipc/pipe_security.h
    core/search/cancel_token.h
    core/search/search_engine.cpp
    core/search/search_engine.h
    core/search/search_types.h
    core/settings/settings.cpp
    core/settings/settings.h
    core/snapshot/compression.cpp
    core/snapshot/compression.h
    core/snapshot/index_io.cpp
    core/snapshot/index_io.h
    core/snapshot/json_lite.cpp
    core/snapshot/json_lite.h
    core/snapshot/object_store.cpp
    core/snapshot/object_store.h
    core/snapshot/secure_dir.cpp
    core/snapshot/secure_dir.h
    core/snapshot/sensitive.cpp
    core/snapshot/sensitive.h
    core/snapshot/snapshot_store.cpp
    core/snapshot/snapshot_store.h
    core/snapshot/snapshot_types.cpp
    core/snapshot/snapshot_types.h
    core/watcher/event_synthesizer.cpp
    core/watcher/event_synthesizer.h
    core/watcher/fs_event.h
    core/watcher/fs_probe.cpp
    core/watcher/fs_probe.h
    core/watcher/rename_tracker.cpp
    core/watcher/rename_tracker.h
    core/watcher/resync.cpp
    core/watcher/resync.h
    core/watcher/self_save_guard.cpp
    core/watcher/self_save_guard.h
    core/watcher/settle_reader.cpp
    core/watcher/settle_reader.h
    core/watcher/watcher_core.cpp
    core/watcher/watcher_core.h
    core/workspace/workspace_model.cpp
    core/workspace/workspace_model.h
    util/atomic_file.cpp
    util/atomic_file.h
    util/encoding.cpp
    util/encoding.h
    util/hash.cpp
    util/hash.h
    util/logger.cpp
    util/logger.h
    util/path_util.h
    util/result.h
    util/task_runner.cpp
    util/task_runner.h
)

target_include_directories(pika_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        # dtl は core/diff/line_diff.cpp の内部実装だけで使う（公開ヘッダに dtl 型を出さない）。
        ${DTL_INCLUDE_DIR}
)

# 自前コードにのみ /W4・/WX を適用（pika_warnings は外部ヘッダに波及しない INTERFACE）。
# md4c は markdown_renderer.cpp 内部だけで使う実装詳細のため PRIVATE。md4c::md4c-html が
# md4c::md4c（コア）を推移リンクする。
target_link_libraries(pika_core
    PRIVATE
        pika_warnings
        md4c::md4c-html
        zstd::libzstd
        # toml11 は core/settings/settings.cpp の内部だけで使う（公開ヘッダに toml 型を出さない）。
        toml11::toml11
        # PCRE2(16bit) は core/search/search_engine.cpp の内部だけで使う（公開ヘッダに PCRE2 型を
        # 出さない＝pcre2-16/UTF で Unicode 文字クラスを扱う。要件5.4）。
        PCRE2::16BIT
        # core/snapshot/secure_dir.cpp が ACL（SID/SDDL）API を使う（要件9.1 の本人のみ ACL）。
        advapi32
    PUBLIC
        xxHash::xxhash
)

# ---- GUI 本体（pika exe）と コンソールスタブ（pika.com）----
# 重量依存（wxWidgets/WebView2）を含むため core-test では構成しない。
if(PIKA_BUILD_GUI)
    add_subdirectory(app)
endif()
