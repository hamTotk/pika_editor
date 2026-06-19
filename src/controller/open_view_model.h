// controller/open_view_model: ファイルを開くときのビュー種別判定（wx 非依存・純ロジック）。
// 要件12.2（I1 画像簡易ビュー・I2 巨大画像ガード・I9 バイナリ非対応表示）/ design.md 10章 B3 /
// spec.md 系統A（縮退判定の GUI 結線素材）/ F-022。
//
// MainFrame::open_file は「テキスト=EditorPanel／画像=ImageViewPanel／巨大画像・バイナリ=
// UnsupportedViewPanel」へ分岐する。その分岐の素値計算（画像拡張子か・ヘッダ寸法・総ピクセル数
// ガード・バイナリ heuristic・寸法不明時のサイズフォールバック）をここに集約し、GUI
// は結果（OpenView） を機械的に消費するだけにする。判定は決定論（同一入力で同一出力）で、wx/FS
// に依存しない。
//
// 依存する純ロジック:
//   - classify_icon（tree_view_model）: 画像拡張子の分類（IconCategory::Image。svg は除外）
//   - util::parse_image_dimensions: ヘッダ寸法（PNG/GIF/BMP。デコードしない）
//   - util::looks_binary: 非テキスト判定（NUL/制御文字比率）
//   - resolve_degrade（degrade_model）: 総ピクセル数ガード（ImageTooLarge）
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pika::controller
{

// open_file の分岐先ビュー種別（GUI はこれを見てページ部品を選ぶ）。
enum class OpenView
{
    Text,          // テキスト＝EditorPanel（従来経路）
    Image,         // ラスター画像＝ImageViewPanel（簡易ビュー。要件12.2 I1）
    ImageTooLarge, // 巨大画像＝UnsupportedViewPanel（デコードせず誘導。要件2.2/12.2 I2）
    Unsupported,   // バイナリ（非テキスト）＝UnsupportedViewPanel（要件12.2 I9）
};

// ビュー種別判定の入力（GUI/プラットフォーム層が集めて渡す素値。すべて wx/FS 非依存）。
struct OpenViewInput
{
    std::string name_or_path;    // ファイル名 or パス（拡張子分類に使う）
    std::string head;            // ファイル先頭チャンク（ヘッダ寸法・バイナリ判定の素材）
    std::uint64_t file_size = 0; // ファイル全体のサイズ（寸法不明画像のフォールバック判定）
    std::uint64_t max_pixels = 60'000'000ull; // 総ピクセル数ガード（settings.render_max_image_px）
    // 寸法不明画像のフォールバック上限（これ以上の画像ファイルはデコードせずガード扱い。
    // JPEG/WEBP/ICO 等ヘッダ解析しない形式で、巨大ファイルの全デコード爆発を避ける。要件2.2）。
    std::uint64_t fallback_max_bytes = 64ull * 1024ull * 1024ull; // 64MB
};

// 判定結果（GUI が機械的に消費する）。
struct OpenViewResult
{
    OpenView view = OpenView::Text;
    // 画像かつ寸法が取れたときの総ピクセル数（0=不明/非画像）。診断・表示の補助に持つ。
    std::uint64_t pixel_count = 0;
};

// 入力からビュー種別を 1 つに解決する純粋関数（F-022）。判定順序:
//   1. 画像拡張子（classify_icon==Image かつ svg 以外）なら画像経路:
//        - ヘッダ寸法が取れて総ピクセル数 > max_pixels → ImageTooLarge（デコードしない）
//        - ヘッダ寸法が取れて上限以下 → Image
//        - ヘッダ寸法が取れない（JPEG/WEBP/ICO 等）→ file_size > fallback_max_bytes なら
//          ImageTooLarge（ガード）、そうでなければ Image（wxImage 通常デコードに委ねる）
//   2. 画像でなく looks_binary(head) なら Unsupported（バイナリ非対応。I9）
//   3. それ以外は Text（EditorPanel・従来経路）
OpenViewResult resolve_open_view(const OpenViewInput& in);

} // namespace pika::controller
