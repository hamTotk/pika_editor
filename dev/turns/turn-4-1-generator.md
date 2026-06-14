# turn-4-1 generator report — sprint 4: core/render（技術リスク(c) の決定論部分）

## スプリント目標（再注入）

技術リスク(c)の決定論部分: `core/render` のホワイトリスト方式サニタイズ・JS依存検知・外部リソース
検知・CSP組み立て・レンダリング暴走ガード判定を UI/WebView2 非依存で実装し gtest で検証する。
spec.md の中心体験「Markdown/HTML をリアルタイムプレビュー」の安全境界（design.md 6章・要件6章）を
コア側で固める。WebView2 実機での順序保証・Resume レイテンシは should/手動チェックリスト側（spec.md
補完判断3）であり本スプリント対象外。

正典の根拠: design.md 6章「WebView2の使い方（安全方針の実装）」の CSP 一元テンプレート・ホワイトリスト
サニタイズ・SVG無害化・CSS遮断、要件2.2「レンダリング暴走ガード（画像6000万px・SVG展開8000万px相当/
要素5万・HTMLタイムアウト10秒）」、要件6.2/6.3「JS検知（HtmlInspector）・外部リソース既定オフ」。

## 変更ファイル一覧

### 新規実装（src/core/render/）
- `render_options.h` — 共有オプション（`RemoteResourcePolicy`・`RenderGuardLimits` 既定値=要件の数値）
- `csp_builder.h` / `csp_builder.cpp` — CSP テンプレート組み立て（design.md 6章を正）
- `html_tokenizer.h` / `html_tokenizer.cpp` — サニタイズ/検知の共通字句解析土台
- `url_classifier.h` / `url_classifier.cpp` — URL 正規化＋危険スキーム/外部スキーム判定（難読化耐性）
- `html_sanitizer.h` / `html_sanitizer.cpp` — ホワイトリスト方式サニタイザ（本体）
- `html_inspector.h` / `html_inspector.cpp` — JS依存検知・外部リソース検知
- `render_guard.h` / `render_guard.cpp` — レンダリング暴走ガード判定（開始前・静的）
- `markdown_renderer.h` / `markdown_renderer.cpp` — md4c GFM 変換＋サニタイズ（should）

### 新規テスト（tests/core/render/）
- `csp_builder_test.cpp` / `html_tokenizer_test.cpp` / `url_classifier_test.cpp`
- `html_sanitizer_test.cpp` / `html_inspector_test.cpp` / `render_guard_test.cpp`
- `markdown_renderer_test.cpp`

### ビルド配線
- `src/CMakeLists.txt` — pika_core に render の 14 ファイルを追加、`find_package(md4c CONFIG REQUIRED)`
  と `md4c::md4c-html` を PRIVATE リンク（markdown_renderer.cpp 内部の実装詳細）
- `tests/CMakeLists.txt` — render の 7 テストを pika_tests に追加（既存の build フィクスチャ依存で実ビルド検証）

vcpkg.json は md4c を既に依存に含むため変更不要。spec.md / sprints.json / ref-dev / eval JSON は未変更。

## must criteria ごとの実装状況（すべて自動テスト化）

| must | 実装 | 検証テスト |
|------|------|-----------|
| ホワイトリストサニタイズ（許可タグ/属性のみ、script・on*・javascript:・iframe/object/embed/base 除去） | `html_sanitizer.cpp`（allowed_tags/forbidden_subtree_tags/forbidden_void_tags、on* 除去、`is_dangerous_url`） | `RemovesScriptTagAndContent` / `RemovesEventHandlerAttributes` / `RemovesJavascriptUrlInHref` / `RemovesObfuscatedJavascriptUrl` / `RemovesIframeObjectEmbedBase` / `AllowsBasicMarkdownTags` |
| インラインSVG無害化（script/foreignObject/イベント属性除去） | svg は許可タグだが script/foreignobject を subtree 除去、on* 除去、SVG属性ホワイトリスト | `NeutralizesInlineSvgScriptAndForeignObject` / `RemovesEventAttributeInsideSvg` |
| CSS遮断（url()・@import） | `is_css_safe`（url(/@import/expression(/javascript:/コメント を含む style を丸ごと落とす）、`<style>` は subtree 除去 | `BlocksCssUrlInStyleAttribute` / `BlocksCssImport` / `KeepsSafeStyleAttribute` |
| JS依存検知（HtmlInspector: script・Tailwind CDN） | `inspect_html`（script タグ・`tailwindcss.com` 参照） | `DetectsScriptTag` / `DetectsTailwindCdnViaScriptSrc` / `DetectsTailwindCdnViaLinkHref` |
| 外部リソース検知（http(s) 画像/CSS/フォント） | `is_external_url`＋style/@import url(http) 走査 | `DetectsExternalImage` / `DetectsExternalCssLink` / `DetectsProtocolRelativeExternal` / `DetectsExternalFontInStyleBlock` / `LocalRelativeImageIsNotExternal` |
| レンダリング暴走ガード（画像6000万px・SVG8000万px/5万要素・HTML要素数/ネスト、開始前判定） | `guard_image`/`guard_svg`/`guard_html`（乗算オーバーフロー・0次元も不可） | `render_guard_test.cpp` 14件（境界=上限ちょうど許可、上限+1 で不可、オーバーフロー不可、設定緩和で許可） |
| CSP（script-src を https://app.pika のみ、遮断時に外部 http(s) を含まない） | `build_csp`（design.md 6章テンプレートを正、object-src/frame-src は default-src 'none' で常時遮断） | `ScriptSrcIsAppPikaOnly` / `BlockedPolicyHasNoExternalHttp` / `DefaultSrcIsNone` / `NeverAllowsObjectOrFrameSrc` |

## should criteria

| should | 状況 |
|--------|------|
| md4c による GFM 変換（テーブル・タスクリスト・打消し線・自動リンク）が呼び出せる | 充足。`render_markdown`（`MD_DIALECT_GITHUB`）＋必ず `sanitize_html`。`markdown_renderer_test.cpp` で GFM 各要素生成と raw HTML script のサニタイズを検証 |
| リモート許可オン時のみ CSP の img-src/font-src/style-src に http: https: を追加 | 充足。`AllowedPolicyAddsExternalToImgFontStyle` で許可オンの追加を、`BlockedPolicyHasNoExternalHttp` で既定オフの非追加を検証 |

## テスト化できなかった criteria

なし（must 7件・should 2件すべて gtest で観測可能に実装）。WebView2 実機が要る順序保証・Resume
レイテンシ・JS有効/無効の直列化は spec.md 補完判断2/3 に従い本スプリント対象外（手動チェックリスト側）。

## 設計判断・申し送り（後続レビュー/スプリントへの可視化）

1. **二重防御の独立性**: サニタイザは CSP（csp_builder）とは独立した壁として設計（design.md 6章 C6）。
   CSP が無効化された環境でもサニタイザ単体で XSS を防ぐ。md4c 出力は必ず `sanitize_html` を通してから
   返し、サニタイズ前 HTML を外へ出さない（XSS 境界を一元化）。
2. **GFM タスクリストの checkbox**: 要件6.2「タスクリスト」描画のため、md4c が出す
   `<input type="checkbox" disabled[ checked]>` の固定形だけを `is_gfm_task_checkbox` で許可し、
   出力も固定形（disabled 強制）に正規化する。入力由来の任意 `<input>` は通さない（テストを緩めるのでなく、
   要件機能を安全に実装した）。当初 `input` を subtree 除去にしていたが、void 要素を subtree 抑制すると
   閉じタグ不在で後続を巻き込む潜在バグになるため、危険 void 要素（embed/base/link/meta/frame）は
   `forbidden_void_tags` で「タグだけ落とす」に分離した。
3. **未許可タグの扱い**: 危険サブツリー（script/style/iframe/object 等）は中身ごと除去。それ以外の
   未知タグはタグだけ落として中身（テキスト）を残しエスケープする（黙ってデータを消さない＝設計原則1）。
4. **`data:` URL**: ナビゲーション系属性（href/src 等）の `data:` は無条件で危険扱い（text/html・script
   実行面を断つ）。壊れた画像プレースホルダの `data:` は pika 生成（別経路）で、CSP の `img-src data:` で
   配信する想定（design.md 6章 I5）。
5. **UI 側へ持ち越す責務**: テンプレート合成（`<base href="https://doc.pika/">`・CSP meta 出力・Mermaid/
   KaTeX/ハイライトの遅延 script 挿入・data-line 注入）、仮想ホストのカスタムリソースハンドラ、JS有効/無効の
   直列化、レンダリングタイムアウト10秒の WebView 内監視は UI/WebView2 スプリントの責務（本モジュールは
   開始前の静的判定・本文 HTML サニタイズまで）。

## 自己実行した verify の結果（合否の正本は run-dev 側の実行）

- `cmake --preset x64-core-test` → exit 0（vcpkg manifest 解決＋構成成立。md4c 解決済み）
- `ctest --preset x64-core-test` → 100% passed（pika_tests_build フィクスチャでビルド→pika_tests 実行）
- 全体: `168 tests from 27 test suites ran. [PASSED] 168`（うち render 新規 72 件 / 7 スイート）
- /W4・/WX 確認: render の 7 cpp を強制再コンパイルして警告・エラーゼロ

DONE: C:/dev/pika_editor/dev/turns/turn-4-1-generator.md
