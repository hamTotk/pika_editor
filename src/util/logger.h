// pika util: 診断ログ。
// design.md 3章 util「ログ」/ 12章「診断ログ（内容を書かない）」。要件12.4。
//
// 記録するのは「パス・操作・エラー」のみ。ファイル内容・選択テキスト・検索語などの
// ユーザーデータは一切書かない（プライバシー保護。診断ログは内容を書かない方針）。
// API は内容を渡せない形にすることで、呼び出し側の事故的な内容混入を構造的に防ぐ。
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace pika::util
{

enum class LogLevel
{
    Info,
    Warn,
    Error
};

class Logger
{
  public:
    // 出力先（sink）を差し替え可能にする（テストではメモリへ、本番ではファイルへ）。
    // 既定 sink は何もしない（ログ無効）。
    using Sink = std::function<void(LogLevel, const std::string& line)>;

    explicit Logger(Sink sink = nullptr);

    // op = 操作名（"open"/"save"/"watch" 等の短い識別子）、path = 対象パス、
    // detail = エラーコード・件数などの非機密メタ（内容は渡さない）。
    // 行は "<level> op=<op> path=<path> detail=<detail>" 形式に整形する。
    void log(LogLevel level, std::string_view op, std::string_view path,
             std::string_view detail = {});

    void info(std::string_view op, std::string_view path, std::string_view detail = {});
    void warn(std::string_view op, std::string_view path, std::string_view detail = {});
    void error(std::string_view op, std::string_view path, std::string_view detail = {});

    // 整形だけ行い文字列を返す（sink を介さない。テストの観測点）。
    static std::string format_line(LogLevel level, std::string_view op, std::string_view path,
                                   std::string_view detail);

  private:
    Sink sink_;
};

} // namespace pika::util
