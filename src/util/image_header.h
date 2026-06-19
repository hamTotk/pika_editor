// util/image_header: ラスター画像のヘッダ寸法解析（デコードしない・wx 非依存）。
// 要件2.2（総ピクセル数ガード）・12.2（巨大画像ガード）/ design.md 10章 B3（画像簡易ビューの
// ピクセルガード）/ CLAUDE.md 設計原則2「固まらない」。
//
// 巨大画像を全デコードするとピクセル数 × 4 バイトのメモリ確保で UI スレッドが固まる/落ちる。
// そこで開く前にヘッダ先頭の固定オフセットだけを読み、幅×高さ（総ピクセル数）を controller の
// 縮退判定（resolve_degrade）へ渡してデコード要否を決める。本ヘルパは PNG/GIF/BMP の固定オフセット
// 解析のみを行い、それ以外（JPEG/WEBP/ICO
// 等の可変長/煩雑なヘッダ）は寸法不明（known=false）を返し、
// 呼び出し側のフォールバック（ファイルサイズ閾値 → wxImage
// 通常デコード）に委ねる（速く作る・足さない）。
//
// 純ロジック（同一バイト列で同一出力）。バイト列が短すぎる/シグネチャ不一致は known=false。
#pragma once

#include <cstdint>
#include <string_view>

namespace pika::util
{

// ヘッダから読んだ画像寸法。known=false なら寸法を取得できなかった（フォールバックへ）。
struct ImageDimensions
{
    bool known = false;       // ヘッダから幅×高さを確定できたか
    std::uint32_t width = 0;  // 幅（ピクセル）
    std::uint32_t height = 0; // 高さ（ピクセル）

    // 総ピクセル数（幅×高さ・64bit でオーバーフローを避ける）。known=false なら 0。
    std::uint64_t pixel_count() const noexcept
    {
        return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    }
};

// バイト列の先頭から PNG/GIF/BMP のヘッダ寸法を読む（デコードしない。要件2.2/12.2）。
// 対応:
//   PNG: 8B シグネチャ後、IHDR の width(offset 16)・height(offset 20)。big-endian uint32。
//   GIF: offset 6-7 に width・8-9 に height（GIF87a/GIF89a）。little-endian uint16。
//   BMP: BITMAPINFOHEADER の width(offset 18)・height(offset 22)。little-endian int32。
//        height は負値（トップダウン DIB）があり得るため絶対値を採る。
// それ以外（JPEG/WEBP/ICO/未知・短すぎ）は known=false を返す（呼び出し側フォールバックへ）。
ImageDimensions parse_image_dimensions(std::string_view bytes);

} // namespace pika::util
