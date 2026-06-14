// core/snapshot: テキスト内容の圧縮（zstd）。
// design.md 7章「objects\<hash> … 内容（zstd圧縮、内容ハッシュ名で格納）」/ 要件9.1「テキスト内容は
// 圧縮して保存する」。
//
// 内容 object はディスク上で zstd 圧縮して保存する。圧縮・復元は対称で、復元結果は原内容と
// バイト一致する（差分・巻き戻しの起点になるため、ロスレスが必須）。
#pragma once

#include "util/result.h"

#include <string>
#include <string_view>

namespace pika::core::snapshot
{

// data を zstd 圧縮する（既定圧縮レベル）。失敗時は Io エラーを返す。
pika::util::Result<std::string> compress(std::string_view data);

// zstd 圧縮済みバイト列を復元する。展開後サイズはフレームヘッダから取得する。
// 壊れたフレーム・サイズ不明フレームは Io エラーを返す（破損 object を黙って空で返さない）。
pika::util::Result<std::string> decompress(std::string_view compressed);

} // namespace pika::core::snapshot
