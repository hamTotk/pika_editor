// pika util: ハッシュ（XXH3）。
// design.md 3章 util「ハッシュ（XXH3）」/ 9章「LF正規化後XXH3」の土台。
// sprint 1 ではバイト列に対する素の XXH3-64 のみを提供する（LF 正規化ハッシュは sprint 2）。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pika::util
{

// バイト列の XXH3 64bit ハッシュ。同一内容に対し決定的な値を返す。
std::uint64_t xxh3_64(std::string_view data) noexcept;

// XXH3-64 を 16 桁の小文字16進文字列で返す（objects 名・index の hash 表記に用いる）。
std::string xxh3_64_hex(std::string_view data);

// CRLF を LF に正規化した内容に対する XXH3-64。
// 未読判定・差分の内容照合は LF 正規化後で行う（改行コードのみの差は未読・差分に出さない。
// design.md 8章・要件8章）。CRLF/LF のみ異なる同一内容は同じ値になる。
// 注: 内部で LF 正規化コピーを確保するため巨大入力・低メモリ時は std::bad_alloc を投げ得る。
// あえて noexcept にしない（noexcept だと bad_alloc が std::terminate＝プロセス即死になり、設計
// 原則1「どの異常系でも復元可能に」に反する）。呼び出し側は必要に応じ捕捉して Result へ変換する。
std::uint64_t xxh3_64_lf(std::string_view data);

// LF 正規化ハッシュの 16 桁小文字16進表記（baselineHash・index の hash 表記）。
std::string xxh3_64_lf_hex(std::string_view data);

} // namespace pika::util
