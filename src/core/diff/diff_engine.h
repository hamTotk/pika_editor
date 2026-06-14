// core/diff: 差分エンジン（累積差分の計算オーケストレーション）。
// design.md 3章 core/diff「`DiffEngine`, `DiffResult`。キャンセル可能」/ 8章「差分・既読の設計」。
// 要件8章。spec.md「中心体験」4「前回確認時点からの累積差分を赤/緑で確認」。
//
// 入力はベースライン内容（前回確認時点のスナップショット内容）と現在内容（ディスク or バッファ）。
// LF 正規化照合の行差分（dtl/Myers）に、変更行ペアの行内強調（語/文字 LCS）を載せて返す。
// 大規模入力は開始前にサイズ判定して計算せずフォールバックする（要件8章「10MB以上自動オフ」/
// design.md 8章 I6「タイムアウトを別スレッド中断に頼らない」）。
//
// 純ロジック（UI/WebView2 非依存）。gtest で決定論検証する。
#pragma once

#include "core/diff/cancel_token.h"
#include "core/diff/diff_types.h"

#include <string_view>

namespace pika::core::diff
{

class DiffEngine
{
  public:
    explicit DiffEngine(DiffLimits limits = {}) : limits_(limits) {}

    // ベースライン内容 old と現在内容 new_content の累積差分を計算する。
    // 入力が DiffLimits 超過なら計算を開始せず DiffResult{truncated=true} を返す。
    // cancel が途中でセットされたら DiffResult{cancelled=true} を返す（協調キャンセル）。
    DiffResult compute(std::string_view old_content, std::string_view new_content,
                       const CancelTokenPtr& cancel = nullptr) const;

    const DiffLimits& limits() const noexcept { return limits_; }

  private:
    DiffLimits limits_;
};

} // namespace pika::core::diff
