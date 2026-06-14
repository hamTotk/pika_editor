// util: UTF-8 std::string と std::filesystem::path の相互変換を一点に集約する。
//
// pika のパス文字列は UTF-8 std::string に統一する契約（CLAUDE.md）。I/O 最下層（util/atomic_file・
// core/snapshot/secure_dir・core/watcher/fs_probe）はこの契約どおり
// MultiByteToWideChar(CP_UTF8,...) で変換する。しかし std::filesystem は、Windows では narrow
// std::string から fs::path を構築すると アクティブ ANSI
// コードページ(CP_ACP)でワイド化し、`.string()` も CP_ACP でエンコードする。 そのため
// `fs::path(utf8_string)` や `path.string()` を介すると UTF-8 が CP_ACP として誤デコードされ、 非
// ASCII を含むパス（日本語ファイル名・ユーザー名を含む %LOCALAPPDATA% 配下のデータルート等）で I/O
// が別パスを指す/失敗する。u8string 経由なら CP_ACP を介さず UTF-8 ⇔ ワイドを正しく往復できる。
//
// 規約: fs::path を組み立てるときは fs::path(std::string)/directory_iterator(std::string) ではなく
// utf8_to_path を、path から UTF-8 文字列を取り出すときは `.string()` ではなく path_to_utf8
// を使う。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pika::util
{

// UTF-8 文字列 → fs::path（CP_ACP を介さない）。
inline std::filesystem::path utf8_to_path(std::string_view s)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// fs::path → UTF-8 文字列（CP_ACP を介さない）。
inline std::string path_to_utf8(const std::filesystem::path& p)
{
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

} // namespace pika::util
