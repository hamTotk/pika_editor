// ui/ui_messages: UI 文言の単一メッセージ定義（ID → 日本語）。
// design.md 10章 K9「ユーザー向け文言は単一のメッセージ定義（ID→日本語文字列）経由で取得し、
// UI クラスに生文字列を直接書かない」/ CLAUDE.md「UI 言語は日本語のみ」。
//
// wx の各クラス（MainFrame・メニュー・ステータス）は生文字列を直書きせず、本ヘッダの ID 経由で
// 日本語文言を取得する。将来の文言変更・多言語化の余地を 1 箇所に閉じる。controller 側の列挙値の
// 文言（状態記号・アイコンラベル）は controller/tree_view_messages が持つ（コアは UI を知らないため
// 分離する）。ここは GUI 固有の文言（メニュー項目・ステータス書式）に限定する。
#pragma once

#include "controller/notification_model.h"

#include <cstddef>
#include <string>

namespace pika::ui
{

// UI 文言 ID（GUI 固有。コア側の列挙値文言は tree_view_messages が担う）。
enum class MsgId
{
    AppTitle,           // ウィンドウ/アプリ名
    MenuFile,           // 「ファイル」
    MenuOpenFolder,     // 「フォルダーを開く...」
    MenuClose,          // 「閉じる」
    MenuExit,           // 「終了」
    MenuSave,           // 「保存」（Ctrl+S。design 5.3）
    MenuConfirm,        // 「確認済みにする」（Ctrl+Enter / Ctrl+Shift+Enter。design 5.4・10章 J3）
    MenuConfirmAll,     // 「すべて確認済みにする」（Ctrl+Alt+Enter。design 5.4・10章 J6）
    MenuRollback,       // 「確認済み時点に戻す」（巻き戻し。design 5.4）
    MenuReview,         // 「レビュー」メニュー見出し（中心体験④。design 10章 J6）
    MenuView,           // 「表示」
    MenuRefresh,        // 「再読み込み」（F5）
    MenuModeSource,     // 「ソース」（表示モード。ui-design 8章）
    MenuModeSplit,      // 「分割」（エディタ＋プレビュー）
    MenuModePreview,    // 「プレビュー」（レンダリング）
    MenuToggleDiff,     // 「差分」トグル（Ctrl+Shift+D）
    MenuHelp,           // 「ヘルプ」
    MenuAbout,          // 「pika について」
    TreePaneTitle,      // ツリーペイン見出し
    StatusReady,        // 起動直後のステータス
    StatusNoFolder,     // フォルダ未オープン時
    StatusWatching,     // 監視中（ReadDirectoryChangesW）
    StatusPolling,      // ポーリングフォールバック中（監視不能環境）
    StatusSyncing,      // 再同期実行中（F5/オーバーフロー回復。進捗表示。design 10章 F3）
    StatusConfirmed,    // 確認済みにした旨（未読解除）
    StatusSaved,        // 保存完了
    StatusLinkNotFound, // プレビュー内リンク先が見つからない（新規空タブを作らず通知。B6/B7・I5）
    NotifyConflict,     // 衝突: 外部内容を退避して上書きした旨（要件7.3。警告色運用）
    NotifyBlockedEncoding,    // 表現不能文字で保存中断（要件5.2・G2）
    NotifyBlockedUnstashable, // 退避不能で保存/巻き戻しをブロック（要件7.3・D3）
    NotifyStashFailed,        // 退避 I/O 失敗で上書き/確認をブロック（データを失わない）
    NotifySaveIoFailed,       // 保存の書き込み失敗（再試行/別名保存/属性確認を促す。失っていない）
    NotifyIndexSaveFailed, // 退避台帳(index.json)の保存失敗（退避 object は保存済み・再操作で復旧）
    NotifyRollbackWriteFailed, // 巻き戻しの書き戻し失敗（退避済みで内容は安全・再試行を促す）
    NotifyConfirmAllSkipped,   // すべて確認済みで一部スキップ（並行変化/退避失敗で未確認が残る）
    NotifyOpenInBrowser,       // 通知バーのボタン: JS 入り HTML を既定ブラウザで開く（B3）
    ConfirmClosePrompt,        // タブを閉じる前の未保存確認（保存/破棄/キャンセル。要件5.7）
    ConfirmExitPrompt,         // 終了前の未保存確認（保存して終了/保存しない/キャンセル。要件5.7）
    ConfirmSave,               // 確認ダイアログのボタン: 保存
    ConfirmSaveAll,            // 確認ダイアログのボタン: すべて保存
    ConfirmDiscard,            // 確認ダイアログのボタン: 破棄
    ConfirmDiscardExit,        // 確認ダイアログのボタン: 保存しない（終了）
    ConfirmCancel,             // 確認ダイアログのボタン: キャンセル
    OverflowNotices,           // 通知バーの「他N件」集約行（{n} は呼び出し側で差し込む）
    EmptyNoFolder,             // 中央: フォルダ未オープンの空状態
    NotificationArea           // 通知バー領域のアクセシブルネーム
};

// ID → 日本語文言（UTF-8）。未定義 ID は空文字を返さず ID 名を返さない（網羅を保つ）。
std::string message(MsgId id);

// ステータスバー右下の書式（未読ファイル数）。要件11章「フォルダ内の未読ファイル数」。
// count=0 のときは「未読なし」を返す。
std::string status_unread(std::size_t count);

// 「すべて確認済みにする」でスキップした件数つきの即時提示文言（全件完了の誤認防止。要件8.3）。
// count=0 のときは空文字を返す（提示しない）。
std::string notify_confirm_all_skipped(std::size_t count);

// 通知バーの「他N件」集約行（design 10章 J1・ui-design 10章）。count=0 のときは空文字を返す。
std::string notify_overflow(std::size_t count);

// 通知バー集約 ViewModel の種別（controller::NotificationKind）→ 日本語の既定文言（UTF-8）。
// NotificationRow.detail が空のとき通知バーが種別から出す文言（design 10章 K9・J1）。
std::string notification_kind_label(controller::NotificationKind kind);

} // namespace pika::ui
