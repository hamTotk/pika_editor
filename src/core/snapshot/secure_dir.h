// core/snapshot: 本人のみアクセス可能な ACL でディレクトリを作成する。
// design.md 7章 / 要件9.1「snapshotsフォルダ・index・内容objectはユーザー本人のみアクセスできる
// 権限（ACL）で作成する」。機密ファイルのハッシュのみ記録と並ぶデータ最小化の一環。
#pragma once

#include <string_view>

namespace pika::core::snapshot
{

// path（UTF-8 絶対パス）のディレクトリを、現在のユーザー（所有者）のみがアクセスできる DACL で
// 作成する。親が無ければ親も作る。既に存在する場合は何もしない（true）。
// 作成に成功すれば true。ACL 設定に失敗してもディレクトリ自体は作成し、継承 ACL のまま true を返す
// （データ保全＞秘匿。退避＝最後の砦の作成を ACL 失敗で諦めない。設計原則1）。
bool create_secure_dir(std::string_view path);

} // namespace pika::core::snapshot
