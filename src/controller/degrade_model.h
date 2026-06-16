// controller/degrade_model: エッジケースの縮退判定（wx 非依存。sprint8 must）。
// spec.md 系統A「縮退判定ロジック」/ 要件12.1（FS 関連）・12.2（非テキスト・画像ガード）・
// 2.2（レンダリング暴走ガード＝総ピクセル数）/ design.md 10章
// B3（画像簡易ビューのピクセルガード）・ 7章 / ui-design 15章（Partial＝機能縮退／Error）/ sprint8
// must#1。
//
// 読み取り専用・権限なし・シンボリックリンク循環・ネットワークドライブ・クラウドプレースホルダ
// （FILE_ATTRIBUTE_OFFLINE / FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS）・画像ピクセル数超過
// の各状況を、
// 「機能を縮退してアプリは起動継続＋次の一手を提示」へ写す決定論ロジック。GUI（系統B）はこの結果
// （DegradeOutcome）を見て、機能の有効/無効と通知バー文言・次の一手ボタンを機械的に出すだけにする。
// 記号文字・日本語文言はここに散らさず列挙（DegradeKind / NextStep）で返す（design 10章 K9）。
// 純ロジック（同一入力で同一出力）。
//
// 設計原則の優先順位（CLAUDE.md）を縮退でも守る:
//   - データを失わない: クラウドプレースホルダはベースライン取得・起動時走査の対象から除外し、
//     フォルダを開いただけでハイドレーション（全ダウンロード）を誘発しない（要件12.1・9章の例外）。
//   - 固まらない:
//   巨大画像はヘッダ寸法で総ピクセル数を判定し、超過ならデコードしない（要件2.2・12.2）。
//   - クラッシュ・フリーズしない:
//   アクセス権なし/排他ロックはリトライ後にエラー表示へ縮退する（継続）。
#pragma once

#include <cstdint>
#include <string_view>

namespace pika::controller
{

// 縮退の種別（要件12.1/12.2 の各エッジケース。値の並びは判定優先順位＝小さいほど先に効く）。
// None は縮退なし（通常表示＝Ideal へ進む）。
enum class DegradeKind
{
    None,             // 縮退なし（通常表示）
    AccessDenied,     // アクセス権なし・他プロセスの排他ロック（リトライ後エラー。要件12.1）
    SymlinkLoop,      // シンボリックリンク/ジャンクションの循環（ツリー無限展開を防ぐ。要件12.1）
    CloudPlaceholder, // クラウドプレースホルダ（オフライン属性。内容読込を遅延。要件12.1・9章）
    ImageTooLarge,    // 画像の総ピクセル数がガード上限超過（デコードせず誘導。要件2.2・12.2）
    NetworkDrive,     // ネットワークドライブ/UNC（監視不能＝ポーリングへ。要件12.1・7章）
    ReadOnly,         // 読み取り専用属性（開けるが保存時に別名保存/属性解除へ誘導。要件12.1）
};

// 縮退時にユーザーへ提示する「次の一手」（行き止まりにしない。ui-design 15章 Empty/Error）。
// GUI はこの列挙からボタン/導線を出す（文言は ui 側の単一メッセージ定義）。
enum class NextStep
{
    None,             // 提示なし（通常表示）
    OpenInDefaultApp, // 「既定のアプリで開く」（巨大画像・非対応。要件12.2）
    RetryOrClose,     // 再試行する/閉じる（アクセス権なし・排他ロック。要件12.1）
    SaveAsOrUnlock,   // 「名前を付けて保存」/属性解除（読み取り専用の保存時。要件12.1）
    PollingNotice,    // 監視→ポーリングへ縮退の通知＋F5 で全体再スキャン（要件12.1・7章）
    OpenOnDemand,     // クラウドプレースホルダ＝開いたときだけ取得する旨（要件12.1）
};

// 縮退判定の入力（GUI/プラットフォーム層が FS メタデータから集めて渡す素値。すべて wx 非依存）。
// 個々のフラグは「列挙時のメタデータのみ」で判定でき、ハイドレーション（内容読込）を誘発しない
// （要件12.1 クラウド対策・design 250行）。
struct DegradeInput
{
    bool access_denied = false; // 権限なし・排他ロックでリトライ後も読めない（要件12.1）
    bool symlink_loop = false;  // 既訪 inode/パスへ戻る循環を検出（ツリー無限展開防止。要件12.1）
    bool cloud_placeholder = false; // FILE_ATTRIBUTE_OFFLINE / RECALL_ON_DATA_ACCESS（要件12.1）
    bool network_drive = false;     // ネットワーク/UNC でディレクトリ監視が機能しない（要件12.1）
    bool read_only = false;         // 読み取り専用属性のファイル（要件12.1）

    // 画像（ラスター）のヘッダ寸法。is_image=true のとき pixel_count を総ピクセル数ガードに掛ける。
    bool is_image = false; // 対象がラスター画像か（.png/.jpg/.gif/.webp/.bmp/.ico。要件12.2）
    std::uint64_t pixel_count = 0; // 幅×高さ（ヘッダから取得・デコード前。要件2.2）
    // 総ピクセル数の既定ガード上限（6000万px。要件2.2。設定で緩和可のため注入する）。
    std::uint64_t max_pixels = 60'000'000ull;
};

// 縮退判定の結果（GUI が機械的に消費する）。
struct DegradeOutcome
{
    DegradeKind kind = DegradeKind::None; // 縮退種別（None＝通常表示）
    NextStep next_step = NextStep::None;  // 提示する次の一手
    bool can_continue = true;    // アプリ継続できるか（縮退してでも常に true＝クラッシュさせない）
    bool blocks_content = false; // 内容のデコード/読込をブロックするか（巨大画像・権限なし・循環）
};

// 入力から縮退結果を 1 つに解決する純粋関数（要件12.1/12.2）。
// 優先順位（DegradeKind の並び・小さいほど先）:
//   AccessDenied（読めない＝最優先で止める）＞ SymlinkLoop（無限展開を止める）＞
//   CloudPlaceholder（内容読込を遅延）＞ ImageTooLarge（デコード爆発を止める）＞
//   NetworkDrive（監視縮退・内容は読める）＞ ReadOnly（開けるが保存時に誘導）。
// ImageTooLarge は is_image かつ pixel_count > max_pixels のときのみ。can_continue は常に true。
DegradeOutcome resolve_degrade(const DegradeInput& in);

// 縮退種別の日本語ラベル（通知バー/ステータス文言。単一のメッセージ定義経由。design 10章 K9）。
std::string_view degrade_kind_label(DegradeKind kind);

// 次の一手の日本語ラベル（ボタン/導線文言。単一のメッセージ定義経由。design 10章 K9）。
std::string_view next_step_label(NextStep step);

} // namespace pika::controller
