// controller/view_state: ビュー別の5状態（UI Stack）解決 ViewModel（wx 非依存。sprint8 must）。
// spec.md 系統A「ビュー別5状態」/ ui-design 15章（Ideal/Empty/Loading/Partial/Error・Empty
// の3分岐）/ 要件10章（状態復元・空状態）・2.2/12章（縮退）/ sprint8 must#2。
//
// 全ビュー共通の表示スタック（Ideal / Empty / Loading / Partial / Error）を決定論的に解決する純粋
// ロジック。GUI（系統B）はこの結果（ViewState＋分岐）を見てプレースホルダ画面・進捗・通知バー・
// 次の一手を機械的に出すだけにする。日本語文言はここに散らさず列挙で返す（design 10章 K9）。
//
// 「機能縮退（Partial/degraded）」と「致命的に表示不能（Error）」を区別する（ui-design 15章末尾）:
//   - Partial:
//   10MB超で差分/プレビュー自動オフ・ベースライン未取得を開いた等＝黙って切らず理由表示。
//   - Error  : WebView2 不在・アクセス権なし・衝突等＝機能を縮退してアプリ継続・次の一手を提示。
// 縮退の具体判定（DegradeKind/NextStep）は controller/degrade_model
// が持ち、本モジュールはその結果を
// ビュー5状態（どの面を出すか）へ畳む（責務分離＝縮退の判定と表示スタックの選択を混ぜない）。
#pragma once

#include "controller/degrade_model.h"

#include <cstdint>
#include <string_view>

namespace pika::controller
{

// ビューの表示スタック（ui-design 15章）。優先度の高い異常から先に解決する。
enum class ViewState
{
    Ideal,   // 通常表示（7〜11章）
    Loading, // ベースライン取得中・列挙中（percent-done＋件数。非ブロック）
    Empty,   // 空状態（行き止まりにしない。CTA＋最近使った項目。3分岐で文言が変わる）
    Partial, // 機能縮退（10MB超で差分/プレビュー自動オフ・ベースライン未取得を開いた）
    Error,   // 致命的に表示不能（WebView2 不在・アクセス権なし・衝突）。アプリは継続
};

// Empty の3分岐（ui-design 15章「3分岐で文言を変える」・要件10章）。文言を変えるための識別子。
enum class EmptyReason
{
    None,           // Empty でない
    NoFolderOpened, // フォルダ未オープン（初回）＝フォルダを開く CTA＋最近使った項目
    SearchNoHits,   // 検索 0 件＝検索条件を変える誘導
    AllConsumed,    // 消化後（全ファイルを確認済みにした後）＝未読なしの達成状態
};

// 5状態解決の入力（GUI/controller が集めて渡す素値。すべて wx 非依存）。
// 状態は上書き優先（Error ＞ Partial ＞ Loading ＞ Empty ＞ Ideal）で解決する。
struct ViewStateInput
{
    // --- Empty 判定（フォルダ/検索/未読の文脈） ---
    bool folder_opened = false;     // ワークスペースフォルダを開いているか（false＝NoFolderOpened）
    bool has_visible_items = false; // ツリー/検索結果に表示項目があるか（folder_opened 後の判定）
    bool is_search_mode = false;    // 検索モードか（true かつ 0件＝SearchNoHits）
    bool all_consumed = false;      // 未読を全消化した直後か（true かつ表示なし＝AllConsumed）

    // --- Loading 判定（列挙中・ベースライン取得中。非ブロック） ---
    bool loading = false;           // 列挙/ベースライン取得が進行中か
    std::uint64_t loaded_count = 0; // 進捗（処理済み件数。percent-done 表示用）
    std::uint64_t total_count = 0;  // 進捗（総件数。0 のとき percent 未確定）

    // --- Partial / Error 判定（縮退結果から畳む） ---
    DegradeOutcome degrade;        // controller/degrade_model の解決結果（None＝縮退なし）
    bool diff_auto_off = false;    // 差分トグルが自動無効化されているか（Partial。ui-design 15章）
    bool baseline_pending = false; // 開いたファイルのベースライン未取得（Partial。ui-design 15章）
};

// 5状態解決の結果（GUI が機械的に消費する）。
struct ViewStateResult
{
    ViewState state = ViewState::Ideal;           // 解決された表示スタック
    EmptyReason empty_reason = EmptyReason::None; // state==Empty のときの3分岐
    DegradeKind degrade_kind = DegradeKind::None; // Partial/Error の縮退種別（None＝なし）
    NextStep next_step = NextStep::None;          // 提示する次の一手（縮退時）
    std::uint64_t loaded_count = 0;               // Loading の進捗（処理済み件数）
    std::uint64_t total_count = 0;                // Loading の進捗（総件数）
};

// 入力から5状態を1つに解決する純粋関数（ui-design 15章。同一入力で同一出力）。
// 解決優先（上書きの強い順）:
//   Error（読込不能＝AccessDenied 等の blocks_content かつ非クラウド・非画像）＞
//   Partial（機能縮退＝差分自動オフ/ベースライン未取得/巨大画像/クラウド/監視縮退）＞
//   Loading（列挙/ベースライン取得中）＞ Empty（表示項目なし）＞ Ideal。
// Empty の分岐は folder_opened/is_search_mode/all_consumed から決める。
ViewStateResult resolve_view_state(const ViewStateInput& in);

// Empty 分岐の日本語文言（行き止まりにしない CTA。単一のメッセージ定義経由。design 10章 K9）。
std::string_view empty_reason_label(EmptyReason reason);

} // namespace pika::controller
