// controller/editor_view_model のテスト（sprint3 系統A）。
// Scintilla 配線決定（EditorConfig 写像・タブ見出し抽出）が wx/FS
// 非依存で決定論的であることを観測する。
// 「原文を変えない（タブ/インデント設定の明示・改行は変換しない）」方針（design 10章
// G3・要件5.2）を固定。
#include "controller/editor_view_model.h"

#include <gtest/gtest.h>

namespace pc = pika::controller;
namespace cs = pika::core::settings;
namespace pu = pika::util;

namespace
{

cs::Settings base_settings()
{
    return cs::default_settings();
}

} // namespace

// tab_width は Settings の値をそのまま int で写す（既定 4）。
TEST(EditorConfigTest, TabWidthFromSettings)
{
    cs::Settings s = base_settings();
    s.tab_width = 8;
    const auto cfg = pc::make_editor_config(s, pu::Newline::Lf);
    EXPECT_EQ(cfg.tab_width, 8);
}

// tab_width=0 は Scintilla で不正（桁送りが進まない）。安全側に 1 へ丸める。
TEST(EditorConfigTest, ZeroTabWidthClampedToOne)
{
    cs::Settings s = base_settings();
    s.tab_width = 0;
    const auto cfg = pc::make_editor_config(s, pu::Newline::Lf);
    EXPECT_EQ(cfg.tab_width, 1);
}

// tab_inserts_spaces=true（空白挿入）→ use_tabs=false（SCI_SETUSETABS の論理反転）。
TEST(EditorConfigTest, InsertSpacesMeansUseTabsFalse)
{
    cs::Settings s = base_settings();
    s.tab_inserts_spaces = true;
    const auto cfg = pc::make_editor_config(s, pu::Newline::Lf);
    EXPECT_FALSE(cfg.use_tabs);
}

// 既定（tab_inserts_spaces=false）はタブ文字を挿入する＝use_tabs=true（原文の体裁を尊重）。
TEST(EditorConfigTest, DefaultKeepsHardTabs)
{
    const auto cfg = pc::make_editor_config(base_settings(), pu::Newline::Lf);
    EXPECT_TRUE(cfg.use_tabs);
}

// 折返し・空白可視化のトグルが素通しで写る。
TEST(EditorConfigTest, WrapAndWhitespaceTogglesPropagate)
{
    cs::Settings s = base_settings();
    s.word_wrap = true;
    s.show_whitespace = true;
    const auto cfg = pc::make_editor_config(s, pu::Newline::Lf);
    EXPECT_TRUE(cfg.word_wrap);
    EXPECT_TRUE(cfg.show_whitespace);
}

// 検出した改行が EOL モードへ写る（CRLF/LF/Mixed/None）。原文の改行は変換しない指標。
TEST(EditorConfigTest, EolModeFollowsDetectedNewline)
{
    EXPECT_EQ(pc::make_editor_config(base_settings(), pu::Newline::Crlf).eol_mode,
              pc::EolMode::Crlf);
    EXPECT_EQ(pc::make_editor_config(base_settings(), pu::Newline::Lf).eol_mode, pc::EolMode::Lf);
    EXPECT_EQ(pc::make_editor_config(base_settings(), pu::Newline::Mixed).eol_mode,
              pc::EolMode::Mixed);
    // 改行なし（None）は LF 既定（新規行の挿入用。原文は空のまま変えない）。
    EXPECT_EQ(pc::make_editor_config(base_settings(), pu::Newline::None).eol_mode, pc::EolMode::Lf);
}

// EditorConfig は表示専用フラグを既定 false で返す（巨大ファイル等の上書きは GUI 側）。
TEST(EditorConfigTest, ReadOnlyDefaultsFalse)
{
    EXPECT_FALSE(pc::make_editor_config(base_settings(), pu::Newline::Lf).read_only);
}

// タブ見出しは絶対パスの最後のセグメント（ファイル名）を取り出す。
TEST(TabTitleTest, ExtractsFileNameFromBackslashPath)
{
    EXPECT_EQ(pc::tab_title_for_path("C:\\work\\proj\\notes.md"), "notes.md");
}

// '/' 区切りでも取り出せる（転送/正規化後の絶対パス）。
TEST(TabTitleTest, ExtractsFileNameFromForwardSlashPath)
{
    EXPECT_EQ(pc::tab_title_for_path("C:/work/proj/README"), "README");
}

// 末尾区切りは無視して直前のセグメントを返す（フォルダ表示名）。
TEST(TabTitleTest, TrailingSeparatorIgnored)
{
    EXPECT_EQ(pc::tab_title_for_path("C:/work/proj/"), "proj");
}

// 区切りが無ければ全体を返す。
TEST(TabTitleTest, NoSeparatorReturnsWhole)
{
    EXPECT_EQ(pc::tab_title_for_path("plain"), "plain");
}

// 空入力・区切りだけの入力は空（呼び出し側が「無題」等へ写す）。
TEST(TabTitleTest, EmptyAndSeparatorOnly)
{
    EXPECT_EQ(pc::tab_title_for_path(""), "");
    EXPECT_EQ(pc::tab_title_for_path("/"), "");
    EXPECT_EQ(pc::tab_title_for_path("\\\\"), "");
}

// 非 ASCII（日本語ファイル名）も UTF-8 バイト列としてそのまま取り出す。
TEST(TabTitleTest, JapaneseFileName)
{
    EXPECT_EQ(pc::tab_title_for_path("C:/作業/メモ.md"), "メモ.md");
}
