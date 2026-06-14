// core/ipc 相対パス正規化の検証（sprint7 must「相対パス正規化: クライアント側で
// 呼び出し元 CWD 基準に絶対パス正規化してから転送する」）。CWD を注入し、相対/ドライブ相対/
// 絶対・`.`/`..` の解決と区切り統一を観測する（サーバー側 CWD 非依存の担保。要件3.2）。
#include "core/ipc/path_normalizer.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::ipc::normalize_to_absolute;

TEST(PathNormalize, RelativeJoinedToCwd)
{
    EXPECT_EQ(normalize_to_absolute("a.md", "C:\\proj"), "C:\\proj\\a.md");
    EXPECT_EQ(normalize_to_absolute("sub\\a.md", "C:\\proj"), "C:\\proj\\sub\\a.md");
}

TEST(PathNormalize, ForwardSlashesUnified)
{
    EXPECT_EQ(normalize_to_absolute("sub/a.md", "C:/proj"), "C:\\proj\\sub\\a.md");
}

TEST(PathNormalize, DotAndDotDotResolved)
{
    EXPECT_EQ(normalize_to_absolute(".\\a.md", "C:\\proj"), "C:\\proj\\a.md");
    EXPECT_EQ(normalize_to_absolute("..\\a.md", "C:\\proj\\sub"), "C:\\proj\\a.md");
    EXPECT_EQ(normalize_to_absolute("..\\..\\a.md", "C:\\proj\\sub\\deep"), "C:\\proj\\a.md");
}

TEST(PathNormalize, DotDotPastRootStopsAtRoot)
{
    // ルートを超える `..` はルートで止める。
    EXPECT_EQ(normalize_to_absolute("..\\..\\..\\a.md", "C:\\proj"), "C:\\a.md");
}

TEST(PathNormalize, AbsolutePathPreservedAndNormalized)
{
    // 既に絶対なら CWD に依らずそのまま正規化する。
    EXPECT_EQ(normalize_to_absolute("D:\\x\\y.md", "C:\\proj"), "D:\\x\\y.md");
    EXPECT_EQ(normalize_to_absolute("D:/x/./y.md", "C:\\proj"), "D:\\x\\y.md");
    EXPECT_EQ(normalize_to_absolute("D:\\x\\sub\\..\\y.md", "C:\\proj"), "D:\\x\\y.md");
}

TEST(PathNormalize, UncAbsolutePreserved)
{
    EXPECT_EQ(normalize_to_absolute("\\\\srv\\share\\a.md", "C:\\proj"), "\\\\srv\\share\\a.md");
    EXPECT_EQ(normalize_to_absolute("\\\\srv\\share\\x\\..\\a.md", "C:\\proj"),
              "\\\\srv\\share\\a.md");
}

TEST(PathNormalize, DriveRelativeSameDriveUsesCwd)
{
    // `C:a.md`（ドライブ相対）は cwd と同じドライブのとき cwd 配下に解決する。
    EXPECT_EQ(normalize_to_absolute("C:a.md", "C:\\proj"), "C:\\proj\\a.md");
}

TEST(PathNormalize, DriveRelativeOtherDriveGoesToRoot)
{
    // cwd と違うドライブの `D:a.md` は指定ドライブのルート直下に置く。
    EXPECT_EQ(normalize_to_absolute("D:a.md", "C:\\proj"), "D:\\a.md");
}

TEST(PathNormalize, NonexistentPathStillNormalized)
{
    // FS にアクセスせず、実在しないパスでも正規化する（新規タブ用）。
    EXPECT_EQ(normalize_to_absolute("new\\file.md", "C:\\proj"), "C:\\proj\\new\\file.md");
}

} // namespace
