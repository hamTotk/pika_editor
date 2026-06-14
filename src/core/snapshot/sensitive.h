// core/snapshot: 機密ファイル判定（内容を保存せず baselineHash のみ記録する対象）。
// design.md 7章 / 要件9.1「.env・*.key・*.pem・*secret* 等は内容を保存せずハッシュのみ記録」。
//
// 機密ファイルは元ファイル削除後に平文コピーが残らないよう object を保存しない（データ最小化）。
// 既定パターンは settings.toml のベースライン除外パターンで調整可だが、本モジュールは既定セットを
// 判定するロジックのみを持つ（設定の読み込みは core/settings の責務）。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pika::core::snapshot
{

// 機密ファイル除外パターン（既定）。要件9.1 の `.env`・`*.key`・`*.pem`・`*secret*`。
std::vector<std::string> default_sensitive_patterns();

// rel_path のファイル名が patterns のいずれかに一致するなら true（大小無視・glob `*` 対応）。
// 一致したファイルは内容 object を保存しない（baselineHash のみ記録）。
bool is_sensitive(std::string_view rel_path, const std::vector<std::string>& patterns);

// 既定パターンでの機密判定（呼び出し側の利便のための薄いラッパ）。
bool is_sensitive_default(std::string_view rel_path);

} // namespace pika::core::snapshot
