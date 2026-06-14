#include "util/logger.h"

#include <utility>

namespace pika::util
{

namespace
{

const char* level_tag(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "INFO";
}

} // namespace

Logger::Logger(Sink sink) : sink_(std::move(sink)) {}

std::string Logger::format_line(LogLevel level, std::string_view op, std::string_view path,
                                std::string_view detail)
{
    std::string line = level_tag(level);
    line += " op=";
    line.append(op);
    line += " path=";
    line.append(path);
    if (!detail.empty())
    {
        line += " detail=";
        line.append(detail);
    }
    return line;
}

void Logger::log(LogLevel level, std::string_view op, std::string_view path,
                 std::string_view detail)
{
    if (!sink_)
    {
        return;
    }
    sink_(level, format_line(level, op, path, detail));
}

void Logger::info(std::string_view op, std::string_view path, std::string_view detail)
{
    log(LogLevel::Info, op, path, detail);
}

void Logger::warn(std::string_view op, std::string_view path, std::string_view detail)
{
    log(LogLevel::Warn, op, path, detail);
}

void Logger::error(std::string_view op, std::string_view path, std::string_view detail)
{
    log(LogLevel::Error, op, path, detail);
}

} // namespace pika::util
