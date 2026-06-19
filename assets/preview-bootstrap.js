/* pika プレビューの同梱スクリプト・ブートストラップ（F-004。https://app.pika/preview-bootstrap.js）。
 * design.md 6章「該当記法がある時だけ <script> を出力・遅延読み込み・per-block の try/catch＋
 * 1秒タイムアウト＋失敗フォールバック＋件数通知」/ 要件6.2/6.4。
 *
 * このスクリプトは pika 生成の信頼済み HTML（サニタイズ済み本文）に対してのみ動く。
 * ユーザー文書由来の JS は実行しない（CSP script-src https://app.pika。design 6章）。
 *
 * 注入は preview_builder が「該当記法がある時だけ」行う。各ライブラリ（mermaid/hljs/renderMathInElement）
 * が読み込まれているかは存在チェックで分岐し、無ければその処理をスキップする（未使用時コストゼロ）。
 *
 * 各ブロックのレンダリングは try/catch ＋ 1 秒タイムアウトで囲み、失敗・タイムアウト時は元の
 * コードブロック表示へ差し戻してエラーバッジを付ける（崩さない＝データを失わない側）。失敗件数は
 * window.chrome.webview.postMessage でネイティブへ通知し、通知バーに連携する。 */
(function () {
    "use strict";

    var PER_BLOCK_TIMEOUT_MS = 1000; // design 6章「1ブロックあたり約1秒」。
    var failures = 0;

    // 1 秒で reject するタイマー付き Promise（per-block タイムアウト）。
    function withTimeout(promise, ms) {
        return new Promise(function (resolve, reject) {
            var timer = setTimeout(function () {
                reject(new Error("render timeout"));
            }, ms);
            Promise.resolve(promise).then(
                function (v) {
                    clearTimeout(timer);
                    resolve(v);
                },
                function (e) {
                    clearTimeout(timer);
                    reject(e);
                }
            );
        });
    }

    // 失敗した <pre><code> を元表示のまま残し、エラーバッジを付ける（崩さないフォールバック）。
    function markFailure(preEl) {
        failures++;
        if (!preEl || preEl.querySelector(".pika-render-error")) {
            return;
        }
        var badge = document.createElement("span");
        badge.className = "pika-render-error";
        badge.textContent = "レンダリング失敗";
        preEl.appendChild(badge);
    }

    // mermaid: code.language-mermaid を 1 ブロックずつ描画し、成功時は図に差し替える。
    function renderMermaid() {
        if (typeof window.mermaid === "undefined") {
            return Promise.resolve();
        }
        try {
            // securityLevel:'strict' で CSP 厳格下でも動く（mermaid 10.x は globalThis があれば
            // Function コンストラクタ分岐に入らず unsafe-eval 不要。design 6章 / F-004 報告参照）。
            // startOnLoad:false で自動走査を止め、pika が per-block 制御する。
            window.mermaid.initialize({ startOnLoad: false, securityLevel: "strict" });
        } catch (e) {
            /* initialize 失敗は描画段で個別フォールバックされる */
        }
        var nodes = document.querySelectorAll("pre > code.language-mermaid");
        var jobs = [];
        for (var i = 0; i < nodes.length; i++) {
            (function (codeEl, idx) {
                var preEl = codeEl.parentElement;
                // mermaid ソースは必ず「デコード済みの生テキスト」で渡す。code 要素の textContent は
                // ブラウザが HTML 実体を 1 段復号した値（`--&gt;` → `-->`）であり、mermaid が要求する
                // 生の図ソースそのもの。innerHTML（`--&gt;` のまま）を渡すと "Syntax error" になる
                // （F-004 の根因）。サニタイザ側が二重エスケープしない（`&gt;` を `&amp;gt;` に
                // しない）ことが前提（core/render/html_sanitizer.cpp）。失敗時は preEl の元表示
                // （可読な `-->` を含む <code>）をそのまま残し、バッジだけ足す（崩さない）。
                var src = codeEl.textContent || "";
                var id = "pika-mermaid-" + idx;
                var job = withTimeout(window.mermaid.render(id, src), PER_BLOCK_TIMEOUT_MS).then(
                    function (out) {
                        var svg = out && out.svg ? out.svg : out;
                        var fig = document.createElement("div");
                        fig.className = "pika-mermaid";
                        fig.innerHTML = svg; // mermaid 生成の信頼済み SVG（securityLevel:strict）。
                        if (preEl && preEl.parentNode) {
                            preEl.parentNode.replaceChild(fig, preEl);
                        }
                    },
                    function () {
                        markFailure(preEl);
                    }
                );
                jobs.push(job);
            })(nodes[i], i);
        }
        return Promise.all(jobs);
    }

    // highlight.js: 言語付き code（mermaid を除く）を 1 ブロックずつハイライトする。
    function renderHighlight() {
        if (typeof window.hljs === "undefined") {
            return;
        }
        var nodes = document.querySelectorAll("pre > code[class*=language-]");
        for (var i = 0; i < nodes.length; i++) {
            var el = nodes[i];
            if (el.className.indexOf("language-mermaid") !== -1) {
                continue; // mermaid は図にするのでハイライト対象外。
            }
            try {
                window.hljs.highlightElement(el);
            } catch (e) {
                markFailure(el.parentElement);
            }
        }
    }

    // 通貨保護用センチネル：renderMathInElement に走査させたくない `$`（直後が数字＝通貨）を
    // 一時退避する不可視マーカ。PUA（私用領域）の文字なので本文・数式・デリミタと衝突しない。
    var USD_SENTINEL = ""; // U+E000/E001（PUA・不可視）。auto-render 後に "$" へ復元する。

    // renderMathInElement は `$` デリミタに「直後が数字なら通貨」ルールを持たないため、
    // 検出側（core/render/preview_features.cpp line_has_math）と整合させる前処理を入れる。
    // ignoredTags（pre/code/script/...）配下のテキストは数式化されないので退避対象から外す
    // （= コード内の `$` を書き換えない。元に戻す処理との対称性も保つ）。
    var MATH_IGNORED_TAGS = {
        SCRIPT: 1,
        NOSCRIPT: 1,
        STYLE: 1,
        TEXTAREA: 1,
        PRE: 1,
        CODE: 1
    };

    // body 配下のテキストノードを走査し、ignoredTags 配下を除いて cb(textNode) を呼ぶ。
    function eachMathTextNode(cb) {
        var walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, {
            acceptNode: function (node) {
                for (var p = node.parentNode; p && p !== document.body; p = p.parentNode) {
                    if (p.nodeType === 1 && MATH_IGNORED_TAGS[p.nodeName]) {
                        return NodeFilter.FILTER_REJECT;
                    }
                }
                return NodeFilter.FILTER_ACCEPT;
            }
        });
        var node;
        var batch = [];
        while ((node = walker.nextNode())) {
            batch.push(node); // 走査中にノードを書き換えると不安定なので一旦集める。
        }
        for (var i = 0; i < batch.length; i++) {
            cb(batch[i]);
        }
    }

    // 「`$` の直後が数字」の `$` を退避（または復元）する。
    function maskCurrency() {
        eachMathTextNode(function (node) {
            var t = node.nodeValue;
            // 直後が ASCII 数字の `$` のみセンチネルへ。本物の数式 `$x=1$` の `$` は対象外。
            var masked = t.replace(/\$(?=[0-9])/g, USD_SENTINEL);
            if (masked !== t) {
                node.nodeValue = masked;
            }
        });
    }
    function unmaskCurrency() {
        eachMathTextNode(function (node) {
            var t = node.nodeValue;
            if (t.indexOf(USD_SENTINEL) !== -1) {
                node.nodeValue = t.split(USD_SENTINEL).join("$");
            }
        });
    }

    // KaTeX: 本文をスキャンして $$...$$（display）と $...$（inline）をレンダリングする。
    // 通貨（`$` 直後が数字）は数式化させない＝検出側ルールと一致（「価格は $5 と $9 です」を崩さない）。
    // 本物の数式（`$x=1$`・`$$E=mc^2$$`）はマスク対象外なので従来どおり描画される。
    function renderMath() {
        if (typeof window.renderMathInElement === "undefined") {
            return;
        }
        maskCurrency(); // auto-render に通貨 `$` を見せない（最小前処理）。
        try {
            window.renderMathInElement(document.body, {
                delimiters: [
                    { left: "$$", right: "$$", display: true },
                    { left: "$", right: "$", display: false },
                    { left: "\\(", right: "\\)", display: false },
                    { left: "\\[", right: "\\]", display: true }
                ],
                throwOnError: false, // KaTeX 構文エラーは赤字フォールバック（崩さない）。
                ignoredTags: ["script", "noscript", "style", "textarea", "pre", "code"]
            });
        } catch (e) {
            failures++; // auto-render 全体の失敗（個別ブロックは throwOnError:false で吸収済み）。
        } finally {
            unmaskCurrency(); // 退避した通貨 `$` を必ず戻す（数式化されなかった素のテキストへ）。
        }
    }

    // 失敗件数をネイティブへ通知する（通知バー連携。design 6章 I1）。
    // wxWebView の AddScriptMessageHandler("pika") で生える window.pika.postMessage を優先し、
    // 無ければ素の WebView2 の window.chrome.webview.postMessage にフォールバックする。
    function notify() {
        if (failures <= 0) {
            return;
        }
        var payload = JSON.stringify({ kind: "render-failures", count: failures });
        try {
            if (window.pika && typeof window.pika.postMessage === "function") {
                window.pika.postMessage(payload);
                return;
            }
            if (
                window.chrome &&
                window.chrome.webview &&
                typeof window.chrome.webview.postMessage === "function"
            ) {
                window.chrome.webview.postMessage(payload);
            }
        } catch (e) {
            /* 通知失敗は致命でない（描画自体は完了している） */
        }
    }

    function run() {
        // 同期処理（hljs/KaTeX）→ 非同期処理（mermaid）の順で実行し、全完了後に件数を通知する。
        renderHighlight();
        renderMath();
        Promise.resolve(renderMermaid()).then(notify, notify);
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", run);
    } else {
        run();
    }
})();
