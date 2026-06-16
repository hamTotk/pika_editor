// controller/editor_view_model: エディタ（Scintilla）配線の wx 非依存決定。
// design.md 10章 G3「原文を変えない（タブ/インデント設定を明示し、勝手に空白へ変換しない）」/
// 要件5.1・5.2（タブ幅・Tab キー挙動・改行コード往復維持）/ spec.md sprint3 must
// （Scintilla 配線・エンコーディング/改行結果の反映）。
//
// wxStyledTextCtrl（Scintilla）への配線そのものは GUI（系統B）だが、「どの設定値を、どの Scintilla
// パラメータへ写すか」「タブ見出しに何を出すか」は wx
// に依存しない決定論的写像である。これを純ロジック として切り出し gtest で観測する（系統A）。GUI
// 側はここで確定した EditorConfig を SCI_SETTABWIDTH / SCI_SETUSETABS / SCI_SETEOLMODE
// へ機械的に流すだけにし、原文を変えない方針をテストで固定する。
#pragma once

#include "core/settings/settings.h"
#include "util/encoding.h"

#include <string>
#include <string_view>

namespace pika::controller
{

// Scintilla の EOL モード（SCI_SETEOLMODE に渡す論理値。実 Win32/Scintilla マクロは GUI
// 側で解決）。 pika
// は改行コードを変換せず原文を維持する（要件5.2）ため、これは「新規行を挿入するときに使う改行」と
// 「表示上の EOL 整形をしない」方針の表明であり、読み込んだ内容の改行は書き換えない。
enum class EolMode
{
    Lf,   // 検出が LF（または改行なし）
    Crlf, // 検出が CRLF
    Mixed // 混在（pika は統一しない。EOL 変換を一切行わない指標）
};

// エディタ 1 タブ分の Scintilla 配線パラメータ（wx 非依存の決定値）。
// GUI（editor_panel）はこの構造体だけを見て Scintilla を設定し、判断ロジックを持たない。
struct EditorConfig
{
    int tab_width = 4;              // SCI_SETTABWIDTH（要件5.1）
    bool use_tabs = true;           // SCI_SETUSETABS（true=タブ文字、false=空白。要件5.1）
    bool word_wrap = false;         // 折返し（要件5.5）
    bool show_whitespace = false;   // 空白/タブ可視化（要件11章）
    EolMode eol_mode = EolMode::Lf; // 検出した改行に追従（新規行の挿入用。原文は変えない）
    bool read_only = false; // 表示専用（巨大ファイル/権限なし等で GUI が後段で上書きしうる）
};

// Settings と読み込んだ改行から、1 タブ分の Scintilla 配線パラメータを決定論的に組み立てる。
// - tab_width: Settings.tab_width（1 未満は安全側に 1 へ丸める。0 幅タブは不正）。
// - use_tabs: Settings.tab_inserts_spaces の論理反転（空白挿入なら use_tabs=false）。
// - eol_mode: util::Newline を EolMode へ写す（Mixed は統一しない指標。要件5.2）。
// 原文の改行・空白は一切変換しない（この設定は「新規入力時の挙動」のみを規定する。design 10章
// G3）。
EditorConfig make_editor_config(const core::settings::Settings& settings, util::Newline detected);

// 絶対パスからタブ見出し（表示名）を取り出す純粋関数。
// 最後のパス区切り（'/' または '\\'）以降をファイル名とする。区切りが無ければ全体を返す。
// 末尾区切りは無視する。空入力は空を返す（呼び出し側が「無題」等へ写す）。
std::string tab_title_for_path(std::string_view absolute_path);

} // namespace pika::controller
