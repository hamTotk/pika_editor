# pika_tests … core/util の単体テスト（gtest）。
# pika_core にリンクし、wx 非依存のコアロジックを決定論検証する（design.md 13章）。

find_package(GTest CONFIG REQUIRED)

add_executable(pika_tests
    controller/app_controller_test.cpp
    controller/data_root_test.cpp
    controller/diff_mode_model_test.cpp
    controller/dir_lister_test.cpp
    controller/document_controller_test.cpp
    controller/editor_view_model_test.cpp
    controller/notification_model_test.cpp
    controller/preview_builder_test.cpp
    controller/restore_plan_test.cpp
    controller/search_session_test.cpp
    controller/settings_view_test.cpp
    controller/shortcut_table_test.cpp
    controller/tab_manager_test.cpp
    controller/tree_view_model_test.cpp
    controller/watch_event_map_test.cpp
    controller/workspace_controller_test.cpp
    core/diff/diff_engine_test.cpp
    core/diff/inline_diff_test.cpp
    core/diff/line_diff_test.cpp
    core/document/review_flow_test.cpp
    core/state/state_io_test.cpp
    core/ipc/cli_parser_test.cpp
    core/ipc/ipc_message_test.cpp
    core/ipc/path_normalizer_test.cpp
    core/ipc/pipe_security_test.cpp
    core/render/csp_builder_test.cpp
    core/render/html_inspector_test.cpp
    core/render/html_sanitizer_test.cpp
    core/render/html_tokenizer_test.cpp
    core/render/markdown_renderer_test.cpp
    core/render/render_guard_test.cpp
    core/render/url_classifier_test.cpp
    core/search/search_engine_test.cpp
    core/settings/settings_test.cpp
    core/snapshot/compression_test.cpp
    core/snapshot/index_io_test.cpp
    core/snapshot/object_store_test.cpp
    core/snapshot/sensitive_test.cpp
    core/snapshot/snapshot_store_test.cpp
    core/watcher/event_synthesizer_test.cpp
    core/watcher/fs_probe_test.cpp
    core/watcher/rename_tracker_test.cpp
    core/watcher/resync_test.cpp
    core/watcher/self_save_guard_test.cpp
    core/watcher/settle_reader_test.cpp
    core/watcher/watcher_core_test.cpp
    core/workspace/workspace_model_test.cpp
    util/atomic_file_test.cpp
    util/encoding_test.cpp
    util/hash_test.cpp
    util/logger_test.cpp
    util/result_test.cpp
    util/task_runner_test.cpp
)

target_link_libraries(pika_tests
    PRIVATE
        pika_warnings
        pika_core
        GTest::gtest
        GTest::gtest_main
)

# verify の契約（spec.md「自動検証」）は `cmake --preset`（構成のみ）→ `ctest --preset` の 2 段で、
# 明示のビルド段がない。新規チェックアウト（build ディレクトリ無し）でも `ctest` 単体で完走するよう、
# ctest 実行時にテスト実体をビルドする setup フィクスチャを登録し、テスト本体をそれに依存させる。
#
# gtest_discover_tests（ビルド時/ctest実行時の自動列挙）は、未ビルドのバイナリから列挙できず
# NOT_BUILT プレースホルダで失敗するため使わない。代わりにテスト実行体を 1 つの ctest テストとして
# 登録し、gtest_main が内部で全 TEST ケースを走らせる（ケース別の合否は gtest 出力で確認できる）。
add_test(
    NAME pika_tests_build
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --config "$<CONFIG>" --target pika_tests
)
set_tests_properties(pika_tests_build PROPERTIES FIXTURES_SETUP pika_tests_fixture)

add_test(
    NAME pika_tests
    COMMAND pika_tests
)
set_tests_properties(pika_tests PROPERTIES FIXTURES_REQUIRED pika_tests_fixture)
