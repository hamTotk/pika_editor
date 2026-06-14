// core/watcher: 自己保存イベントの抑制。
// design.md 5.2「自己保存の抑制」。要件7章。sprint3 must「自己保存抑制」。
//
// pika が保存する直前に「保存トークン」（パス＋保存後ハッシュ）を登録する。watcher が変更イベントを
// 処理するとき、現ディスク内容のハッシュが保存後ハッシュと一致する場合のみ自己イベントとして
// 1 回だけ消費（ワンショット）する。時刻窓は古いトークンを GC するための補助的安全弁に過ぎず、
// 窓を超過してもハッシュが一致すれば自己保存として抑制し、内容が異なれば外部変更として扱う。
//
// 純ロジック（現ディスク内容のハッシュは呼び出し側が渡す）。gtest で決定論検証する。
#pragma once

#include "core/watcher/fs_event.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pika::core::watcher
{

// 1 つの保存に対する保存トークン。複数回の保存で同一パスに複数登録されうるため、
// パスごとに「保存後ハッシュ → 登録時刻」を持つ。ハッシュ一致を主条件・時刻窓を補助とする。
class SelfSaveGuard
{
  public:
    // 既定の時刻窓（補助的安全弁。ms）。窓を超えても一致すれば抑制するため厳密でなくてよい。
    static constexpr TimeMs kDefaultWindowMs = 5000;

    explicit SelfSaveGuard(TimeMs window_ms = kDefaultWindowMs) : window_ms_(window_ms) {}

    // 保存直前に呼ぶ。path への保存後内容の LF 正規化ハッシュ（hash_lf）を登録する。
    // hash_lf は正規の値として扱う（0 を「計算不能」のセンチネルにしない）。保存後ハッシュが
    // 取れない場合は呼び出し側が register_save を呼ばないこと（WatcherCore::HashProbe
    // は読み取り側を std::optional にして「計算不能(nullopt)」と「値0」を型で分離する）。
    void register_save(const std::string& path, std::uint64_t hash_lf, TimeMs at);

    // 変更イベントを自己保存として消し込むか判定する。
    // 現ディスク内容の LF 正規化ハッシュ（disk_hash_lf）が登録済みハッシュと一致する場合のみ
    // true を返し、そのトークンを消費（ワンショット）する。一致しなければ false（外部変更）。
    // 時刻は古いトークンの GC 判定にのみ使う（窓超過でも一致すれば抑制する）。
    bool consume_if_self(const std::string& path, std::uint64_t disk_hash_lf, TimeMs now);

    // 補助的 GC。now から window_ms_ を超えて古いトークンを破棄する。
    // 主条件はハッシュ一致なので必須ではないが、保存後に外部が即上書きしてハッシュが永遠に
    // 一致しないトークンの滞留を防ぐ（メモリの安全弁）。
    void gc(TimeMs now);

    // 現在保持しているトークン数（テスト・診断用）。
    std::size_t pending_count() const noexcept;

  private:
    struct Token
    {
        std::uint64_t hash_lf = 0;
        TimeMs at = 0;
    };

    // パス → 登録順のトークン列（同一パスへの連続保存に対応）。
    std::unordered_map<std::string, std::vector<Token>> tokens_;
    TimeMs window_ms_;
};

} // namespace pika::core::watcher
