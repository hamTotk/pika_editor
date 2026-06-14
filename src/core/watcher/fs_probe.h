// core/watcher: ファイルの軽量メタ取得（mtime・サイズ）と内容ハッシュ。
// design.md 5.2「確定読み」「バッファオーバーフロー回復」/ 9章「mtime+サイズプレスクリーン→
// 不一致のみハッシュ」。要件7章・9章。
//
// 確定読み（中途内容の防止）とオーバーフロー再同期はディスクの実メタを参照する。プレスクリーンは
// mtime+サイズで、内容ハッシュは不一致時のみ計算する（ハイドレーション・ハッシュコストを避ける）。
// 本ヘッダの関数は実 FS を触る（gtest はテンポラリフォルダで検証する。design.md 13章 F2/F6）。
#pragma once

#include "util/result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pika::core::watcher
{

// ファイルの軽量メタ。存在しなければ exists=false。
struct FileStat
{
    bool exists = false;
    std::uint64_t size = 0;     // バイト数
    std::uint64_t mtime_ns = 0; // 最終更新時刻（100ns 単位の Win32 FILETIME 相当）
};

// path のメタを取得する（内容は読まない）。取得失敗（権限等）でも例外は投げず、
// exists=false の FileStat を返す（監視は止めない。安全側）。
FileStat probe(std::string_view path);

// path の内容の LF 正規化ハッシュを返す（不一致が疑われたファイルにのみ呼ぶ）。
// 読み取り失敗（共有違反・権限・不在）は err を返す（呼び出し側がリトライ/保留を決める）。
pika::util::Result<std::uint64_t> content_hash_lf(std::string_view path);

} // namespace pika::core::watcher
