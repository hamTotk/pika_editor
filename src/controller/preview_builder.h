// controller/preview_builder: プレビュー/差分の完全HTML文書を組み立てる（wx 非依存）。
// design.md 5.5（テンプレート合成）・6章（CSP/仮想ホスト/base）・8章（差分の色非依存 +/-）/
// ui-design 8/11章（差分は unified・色＋記号＋行内下線）/ spec.md sprint5 must。
//
// core/render の render_markdown（サニタイズ済み本文HTML）と core/diff の DiffResult を入力に、
// CSP メタ・base href・スクリプト挿入を載せた 1 枚の完全HTML文書文字列を作る。WebView2 へは
// この文字列をそのままナビゲートする（GUI＝系統B）。HTML 組み立ては純文字列ロジックなので、
// CSP の付与・色非依存 +/- クラス・grid（プレビュー＋差分）・HTML エスケープを gtest で観測する。
#pragma once

#include "core/diff/diff_types.h"
#include "core/render/preview_features.h"
#include "core/render/render_options.h"

#include <string>
#include <string_view>

namespace pika::controller
{

// 文書内容の種類（JS 有効/無効と差分対象の出し分け。design 6章の表）。
enum class PreviewKind
{
    Markdown, // Markdown プレビュー（JS 有効。同梱アセットのみ）
    Html,     // HTML プレビュー（JS 無効＝IsScriptEnabled=false 相当）
};

// ファイル名/拡張子からプレビュー種別を分類する純粋ロジック（JS 有効/無効の出し分け根拠）。
// .md/.markdown は Markdown（JS 有効）、.html/.htm は Html（JS 無効）。design 6章の表。
// 既定（差分面・分類不能）は Markdown 相当（pika 生成の信頼済み HTML）として扱う。
PreviewKind classify_preview_kind(std::string_view name_or_path);

// 差分対象になり得る種別か（.md/.markdown/.html/.htm/.svg。ui-design 8章「.md/.html/.svg」）。
// それ以外（画像・バイナリ等）は差分トグルを出さない（DiffDisableReason::NotDiffableType の入力）。
bool is_diffable_type(std::string_view name_or_path);

// プレビュー/差分 HTML 文書の組み立て入力。表示色は持たない（CSS が色を付ける。色非依存）。
struct PreviewDoc
{
    PreviewKind kind = PreviewKind::Markdown;
    // サニタイズ済み本文 HTML（core/render::render_markdown の結果 or サニタイズ済み HTML 本文）。
    // 呼び出し側でサニタイズ済みのものだけを渡す（生 md4c 出力をここへ入れない）。
    std::string body_html;
    // リモートリソース許可状態（既定 Blocked。CSP に外部 http(s) を含めるかの切替）。
    core::render::RemoteResourcePolicy remote_policy = core::render::RemoteResourcePolicy::Blocked;
    // 本文が要する同梱機能（Mermaid/KaTeX/highlight.js）。該当時のみ <script>/<link> を注入する
    // （未使用時コストゼロ＝設計原則③。design 6章「該当記法がある時だけ <script> を出力」）。
    // 既定（全 false）では一切の vendor 注入をしない（素の Markdown プレビューと同コスト）。
    core::render::PreviewFeatures features;
};

// プレーンテキストを HTML エスケープする（差分行の原文を安全に埋め込む。XSS 経路を作らない）。
// &<>" を実体参照へ。差分原文はユーザー文書由来のため必ず通す。
std::string escape_html(std::string_view text);

// CSP メタタグ（design 6章 CSP テンプレート）を含む <head> を組み立てて返す。
// base href="https://doc.pika/"（相対画像/リンク解決）と app.pika のスタイルを参照する。
// features に該当機能があれば、その CSS（KaTeX/highlight.js テーマ）の <link> も <head> に足す
// （該当時のみ＝未使用時コストゼロ。design 6章・原則③）。
std::string build_head(core::render::RemoteResourcePolicy policy,
                       const core::render::PreviewFeatures& features = {});

// features に該当する同梱 JS（vendor）＋ブートストラップの <script> を </body> 直前用に組み立てる。
// 該当機能が無ければ空文字を返す（一切注入しない＝未使用時コストゼロ）。スクリプトは app.pika 仮想
// ホストから配信し、読み込み順（vendor → preview-bootstrap）を担保する（design 6章 I1）。
std::string build_feature_scripts(const core::render::PreviewFeatures& features);

// 差分結果を unified 差分 HTML（行ごとに +/-/' ' を色非依存クラスで表す）へ変換する。
// 各行は <div class="diff-line diff-add|diff-del|diff-ctx"> で、行頭マーカ（+/-/空白）を必ず出す
// （色だけに依存させない。要件8.4）。truncated 時は「大きすぎる」旨の最小情報のみを出す。
std::string build_diff_body(const core::diff::DiffResult& diff);

// プレビュー文書（body_html）を完全HTML文書文字列に組み上げる（CSP・base・本文）。
std::string build_preview_document(const PreviewDoc& doc);

// 差分文書（diff のみ。ソース＋差分ON / 分割＋差分ON の差分面）を完全HTML文書文字列に組み上げる。
std::string build_diff_document(const core::diff::DiffResult& diff,
                                core::render::RemoteResourcePolicy policy);

// プレビュー＋差分ON（左プレビュー・右差分を grid で横並び。design 6章）を 1 枚の完全HTML文書に。
// 左 = サニタイズ済み本文 HTML、右 = unified 差分 HTML。左右は独立スクロール（CSS が担う）。
std::string build_preview_diff_grid_document(const PreviewDoc& doc,
                                             const core::diff::DiffResult& diff);

} // namespace pika::controller
