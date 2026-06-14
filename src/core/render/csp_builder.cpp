#include "core/render/csp_builder.h"

namespace pika::core::render
{

namespace
{

// 同梱アセットの仮想ホスト（pika 生成の信頼済み JS/CSS/フォントのみ）。
constexpr const char* kAppHost = "https://app.pika";
// 文書フォルダの仮想ホスト（相対画像の解決用カスタムリソースハンドラ）。
constexpr const char* kDocHost = "https://doc.pika";

} // namespace

std::string build_csp(RemoteResourcePolicy policy)
{
    // design.md 6章の一元定義テンプレートを正とする：
    //   default-src 'none'; script-src https://app.pika;
    //   style-src https://app.pika 'unsafe-inline'; font-src https://app.pika;
    //   img-src https://app.pika https://doc.pika data:
    // object-src / frame-src は default-src 'none' により常時遮断（明示追加しない）。
    const bool remote = (policy == RemoteResourcePolicy::Allowed);

    std::string csp;
    csp.reserve(256);

    // default-src は常に 'none'（許可ディレクティブを明示列挙する基盤）。
    csp += "default-src 'none'; ";

    // script-src は常に同梱アセットのみ。リモート許可状態に関係なく外部 http(s) を足さない
    // （ユーザー文書由来 JS を実行しない境界。sprint4 must）。
    csp += "script-src ";
    csp += kAppHost;
    csp += "; ";

    // style-src：同梱アセット＋インライン CSS（テンプレート由来の信頼済み style）。
    // リモート許可時のみ外部 http: https: を追加する。
    csp += "style-src ";
    csp += kAppHost;
    csp += " 'unsafe-inline'";
    if (remote)
    {
        csp += " https: http:";
    }
    csp += "; ";

    // font-src：同梱アセットのみ。リモート許可時のみ外部を追加。
    csp += "font-src ";
    csp += kAppHost;
    if (remote)
    {
        csp += " https: http:";
    }
    csp += "; ";

    // img-src：同梱＋文書フォルダ＋プレースホルダ用 data:。リモート許可時のみ外部を追加。
    csp += "img-src ";
    csp += kAppHost;
    csp += " ";
    csp += kDocHost;
    csp += " data:";
    if (remote)
    {
        csp += " https: http:";
    }
    csp += "; ";

    // base-uri / form-action / frame-ancestors は default-src 'none'
    // が及ばないため、ポリシー非依存で 常時 'none' を出力する。万一サニタイズが破られても <base>
    // による相対URL基底すり替え・<form action> での外部送信・フレーム埋め込みを CSP
    // 単独で止める（二重防御の対称化。design.md 6章 C6）。
    csp += "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

    return csp;
}

} // namespace pika::core::render
