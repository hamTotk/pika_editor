// controller/notification_model: 通知バー集約 ViewModel（wx 非依存。sprint7 must）。
// spec.md 系統A「通知バー集約 ViewModel」/ design.md 10章 J1（最大3本＋他N件・優先順位・集約・
// タブ固有/グローバル）/ ui-design 10章（最大3本＋「他N件」集約・モノトーン・衝突のみ警告色）/
// 要件11.1 / sprint7 must#2。
//
// 非モーダル・複数キューの通知を決定論的に集約する純粋ロジック。GUI（系統B）の NotificationBar は
// この結果（NotificationView）を機械的に描画するだけにし、優先順位・集約・件数集計の判断はここへ
// 集約する。記号文字や日本語文言はここに散らさず種別（NotificationKind）で返し、表示文字列への写像は
// ui 側の単一メッセージ定義が担う（design 10章 K9）。
//
// 集約規則（design 10章 J1 / ui-design 10章）:
//   - 同時表示は最大 3 本。超過分は「他N件」へ畳む（N=表示しきれなかった件数）。
//   - 優先順位（高い順）: 衝突 ＞ 設定エラー ＞ 外部リソース参照 ＞ JS検知 ＞ 巨大ファイル。
//     同種別内は新しいもの（seq が大きい）を優先する。
//   - 同一ファイル・同一種別は最新（seq 最大）の 1 件へ集約する（古い同種を畳む）。
//   - グローバル通知（tab 非依存。設定エラー等）と
//   タブ固有通知（特定ファイルに紐づく）を切替表示する：
//     アクティブタブのファイルに紐づく通知＋グローバル通知のみを対象にし、他タブ固有の通知は出さない。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pika::controller
{

// 通知の種別（design 10章 J1 の優先順位カテゴリ）。値の並びが優先順位（小さいほど高優先）。
// 衝突のみデータ損失リスクのため警告色運用、他はモノトーン（ui-design
// 10章）。色非依存で種別を持つ。
enum class NotificationKind
{
    Conflict,       // 衝突（外部内容を退避して上書き）＝最優先・警告色（要件7.3）
    SettingsError,  // settings.toml の不正値（既定フォールバック警告。要件10.3）
    RemoteResource, // 外部リソース参照（既定オフのプレビューが外部 http(s) を参照。要件6章）
    JsDetected,     // ユーザー文書由来 JS の検知（HtmlInspector。要件6章）
    BigFile,        // 巨大ファイル段階制で機能縮退（10MB/200MB。要件2.2・9.2）
};

// 通知 1 件（GUI が集める素材）。tab_path が空ならグローバル通知（タブ非依存）。
struct Notification
{
    NotificationKind kind = NotificationKind::Conflict;
    std::string tab_path; // 紐づくファイル絶対パス（空＝グローバル）
    std::uint64_t seq =
        0; // 投入順序の単調増加 ID（新しいほど大。同種・同一ファイルの最新判定に使う）
    // 表示メッセージ本体（任意。空なら ui
    // 側が種別から既定文言を出す）。内容は書かない（診断ログ規約）。
    std::string detail;
};

// 集約結果 1 行（NotificationBar が描画する単位）。
struct NotificationRow
{
    NotificationKind kind = NotificationKind::Conflict;
    std::string tab_path; // グローバルは空
    std::uint64_t seq = 0;
    std::string detail;
    bool is_global = false; // tab_path 空＝グローバル
};

// 集約後の通知ビュー（NotificationBar が機械的に描画する）。
struct NotificationView
{
    std::vector<NotificationRow> rows; // 最大 kMaxVisible 本（優先順位＋最新で整列）
    std::size_t overflow = 0;          // 「他N件」の N（畳んだ件数。0 なら集約表示しない）
};

// 同時表示する最大本数（design 10章 J1 / ui-design 10章「最大3本」）。
inline constexpr std::size_t kMaxVisible = 3;

// 通知集合をアクティブタブ文脈で集約して NotificationView を返す純粋関数（同一入力で同一出力）。
//   - active_tab_path に紐づくタブ固有通知＋グローバル通知のみを対象にする（他タブ固有は除外）。
//   - 同一ファイル・同一種別は最新（seq 最大）の 1 件へ集約。
//   - 優先順位（Conflict>SettingsError>RemoteResource>JsDetected>BigFile）→同種は seq 降順で整列。
//   - 先頭 kMaxVisible 本を rows に、超過分の件数を overflow に積む。
NotificationView aggregate_notifications(const std::vector<Notification>& notifications,
                                         const std::string& active_tab_path);

} // namespace pika::controller
