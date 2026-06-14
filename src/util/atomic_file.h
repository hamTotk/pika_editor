// pika util: アトミック書き込み（一時ファイル → rename）。
// design.md 5.3「書き込みは一時ファイル → ReplaceFile（アトミック・属性/ACL維持）」/
// 12章「すべての永続化ファイルは一時ファイル→rename
// のアトミック書き込み」。要件12.1（クラッシュ耐性）。
//
// 同一ディレクトリに一時ファイルを作って全内容を書き、fsync 相当の後に最終パスへ rename する。
// 書き込み途中でクラッシュしても、最終パスの旧内容は破損しない（rename はアトミック）。
#pragma once

#include "util/result.h"

#include <string>
#include <string_view>

namespace pika::util
{

// bytes を path へアトミックに書き込む。
// 既存ファイルがある場合は内容・属性を保ったまま置換する（ReplaceFile 経路）。
// 一時ファイルは path と同一ディレクトリに作る（別ボリュームへの rename はアトミックでないため）。
// 途中失敗時は一時ファイルを削除し、最終パスの旧内容は変更しない。
Result<void> write_atomic(std::string_view path, std::string_view bytes);

// ファイル全体を読み込む（バイト列をそのまま返す。エンコーディング変換はしない）。
// 読み込み・I/O の対称ヘルパ（テストとアトミック往復確認に使う）。
Result<std::string> read_all(std::string_view path);

} // namespace pika::util
