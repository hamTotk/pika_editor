// ui/preview_view: 共有 1 枚 WebView2 のプレビュー/差分ファサード（wx 依存・系統B）。
// design.md 6章（1枚共有・仮想ホスト・CSP・JS有効無効の直列切替・ナビゲーション制御・TrySuspend）/
// 5.5（プレビュー更新）/ ui-design 8/11章 / spec.md sprint5 must。
//
// 全タブ・全モードで wxWebView を 1 枚だけ共有する（タブ数に比例した WebView2 コストを出さない）。
// HTML の組み立て（CSP/base/色非依存差分）は controller::preview_builder（wx 非依存・gtest 済み）が
// 行い、本クラスは「どの HTML をナビゲートし・占有世代を照合し・リンクを振り分けるか」の実配線に
// 徹する。判断ロジックは controller 側にあり、ここは GUI 実機（系統C）でのみ最終検証する。
#pragma once

#include "controller/diff_mode_model.h"
#include "controller/preview_builder.h"
#include "core/diff/diff_types.h"
#include "core/render/render_options.h"

#include <wx/panel.h>

#include <functional>
#include <memory>
#include <string>

class wxWebView;
class wxWebViewEvent;

namespace pika::ui
{

// プレビュー内リンクの振り分け要求（design 6章）。ナビゲーションを全キャンセルし pika 側へ。
// 相対 .md/.html はタブで開き、他は既定ブラウザへ（実施は MainFrame）。
using NavigateRequest = std::function<void(const std::string& url)>;

// 共有 1 枚 WebView2 を所有し、プレビュー/差分を出し分けるパネル。
// 初回プレビュー要求まで wxWebView を生成しない（遅延初期化＝軽量原則。design 5.1 手順5）。
class PreviewView : public wxPanel
{
  public:
    PreviewView(wxWindow* parent, wxWindowID id = wxID_ANY);
    ~PreviewView() override;

    // WebView2 ランタイムが使えるか（差分トグル可否判定 evaluate_diff_toggle へ渡す。design 6章）。
    static bool webview_available();

    // 表示中文書の親フォルダ（doc.pika カスタムリソースハンドラの許可範囲。'/' 区切り絶対パス）。
    // 空なら doc.pika 要求をすべて拒否する（対象消滅時のマッピング解除）。
    void set_document_root(const std::string& folder_abs);

    // リンク振り分けコールバックを登録する（ナビゲーションインターセプト時に呼ぶ）。
    void set_on_navigate(NavigateRequest cb) { on_navigate_ = std::move(cb); }

    // プレビュー（差分OFF）を表示する。占有鍵 key で世代を進め、HTML をナビゲートする。
    void show_preview(const controller::OccupancyKey& key, const controller::PreviewDoc& doc);

    // 差分面のみ（ソース＋差分ON / 分割＋差分ON）を表示する。
    void show_diff(const controller::OccupancyKey& key, const core::diff::DiffResult& diff,
                   core::render::RemoteResourcePolicy policy);

    // プレビュー＋差分ON（左プレビュー・右差分の grid。1枚WebView2内）を表示する。
    void show_preview_diff_grid(const controller::OccupancyKey& key,
                                const controller::PreviewDoc& doc,
                                const core::diff::DiffResult& diff);

    // 全タブが非プレビューで一定時間経過したら呼ぶ（TrySuspend。design 6章 B1/DEC-02）。
    void suspend_if_idle();

    // 現在の占有世代（ワーカー結果適用前照合に使う。design 4章）。
    std::uint64_t generation() const noexcept { return occupancy_.generation(); }

  private:
    // 遅延生成：初回プレビュー要求でだけ wxWebView を作る（仮想ホスト/ハンドラ/設定もここで）。
    void ensure_webview();
    // 完全HTML文書をナビゲートする。kind で JS 有効/無効を直列（前ナビ完了待ち）で切替える。
    void navigate(const std::string& full_html, controller::PreviewKind kind);
    void on_navigating(wxWebViewEvent& evt); // 全キャンセルし on_navigate_ へ振り分ける
    void on_loaded(wxWebViewEvent& evt);     // NavigationCompleted 相当（直列化の解除点）

    wxWebView* web_ = nullptr; // 共有 1 枚（遅延生成。nullptr＝未生成）
    controller::OccupancyTracker occupancy_;
    std::string doc_root_; // doc.pika の許可フォルダ（'/' 区切り絶対パス・空＝拒否）
    NavigateRequest on_navigate_;
    bool js_enabled_ = true; // 現在の JS 有効状態（Markdown/差分=有効・HTML=無効）
    bool suspended_ = false; // TrySuspend 済みか（次回表示で Resume）
};

} // namespace pika::ui
