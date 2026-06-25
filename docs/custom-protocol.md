# custom protocol 解説（プレビュー配信の仕組み）

pika のプレビュー（未信頼文書の描画）は **custom protocol** という仕組みで HTML を配信する。
「ローカルにサーバーを立てているのでは？」と誤解されやすいが、**サーバーではない**。
本書はその仕組みと、pika での実装箇所をまとめる。

> 正典は `docs/specs/2026-06-20-tauri-rewrite-architecture-design.md`（6章）。本書はその実装視点の補足。

---

## 1. ひとことで言うと

custom protocol は **Tauri（その下の wry / WebView2）が提供する仕組み**で、
WebView が `pika-preview://…` というURLにアクセスしたとき、
**ネットワークに出さず、プロセス内の Rust 関数を直接呼ぶ**もの。

- TCP ポートを開かない（＝本物のサーバーではない）
- 通信の実体は「同じプロセス内の関数呼び出し」
- 外部プロセスから到達できない
- 形は HTTP（`Request` / `Response`）だが、中身はメモリの受け渡し

---

## 2. 「普通のローカルサーバー」との違い

### ① 普通のローカルサーバー（例：開発時の Vite dev server）

```
┌────────────────────── PC ──────────────────────┐
│  pika プロセス              Vite プロセス（別物）│
│  ┌──────────┐   TCP/IP     ┌──────────────────┐ │
│  │ WebView2 │ ───────────► │ HTTP サーバー     │ │
│  │          │ ◄─────────── │ :5173 を LISTEN   │ │
│  └──────────┘  ソケット通信 └──────────────────┘ │
│                    ▲  ポート5173が「開いている」  │
└────────────────────┼────────────────────────────┘
                     │ 他アプリ/ブラウザからも
              http://localhost:5173 で到達できてしまう
```

- OS にポートを予約させる＝本物のサーバー。
- リリースビルドでは消える（`dist/` の静的ファイルを WebView が直接読む）。

### ② custom protocol（`pika-preview://`）

```
┌──────────────── pika プロセス（1個だけ）────────────────┐
│  別WebView（権限ゼロ）              Rust コア            │
│  ┌────────────────────┐          ┌────────────────────┐ │
│  │ navigate(          │ ①横取り   │ handle_preview_    │ │
│  │  "http://pika-     │ ────────► │ request            │ │
│  │   preview.localhost│ ※ネット   │  ②パスで分岐        │ │
│  │   /doc/5")         │   に出ない │   /doc/5 →HTML返す  │ │
│  │   描画 ◄───────────│ ◄──────── │ ③Response（メモリ） │ │
│  └────────────────────┘          └────────────────────┘ │
│  ※①②③はすべて同一プロセス内の関数呼び出し／ポート無し  │
└──────────────────────────────────────────────────────────┘
        ▲ 他プロセスからは到達不能（ポートが無いので狙えない）
```

### 対比表

| | 普通のローカルサーバー | custom protocol |
|---|---|---|
| TCP ポート | 開く（例 :5173） | 開かない |
| 通信の実体 | ソケット越しの I/O | 同一プロセス内の関数呼び出し |
| 外部から到達 | できてしまう | できない |
| `http://…` の見た目 | 本物の HTTP | 見た目だけ（WebView2 が内部解決） |
| 速度 | シリアライズ＋ソケット往復 | メモリ受け渡しで安い |

### 「`http://pika-preview.localhost`」という紛らわしい名前について

Windows では Tauri/WebView2 の仕様で custom protocol が `http://<scheme>.localhost/` という
**見た目**に化けるだけ。`localhost` と書いてあっても **127.0.0.1 に接続しているわけではない**。
WebView2 が「この `.localhost` は登録済みハンドラ行き」と判断し、ソケットを使わず Rust の関数へ回す。

---

## 3. なぜ pika で custom protocol を使うのか

pika は「AIエージェントの出力物＝未信頼な HTML/Markdown」を表示するツール。
素朴に作るなら、サニタイズした HTML を `invoke` でフロントに渡して `innerHTML` で描けばよさそうに見える。
しかしそれは危険・非効率なので、custom protocol を使う。理由は3つ。

1. **オリジン分離（最重要・セキュリティ）**
   未信頼 HTML を **権限ゼロの別WebView**（capability を一切与えない＝Tauri API に到達不能）へ、
   custom protocol で Rust から直接流し込む。別WebView は `pika-preview.localhost` という**別オリジン**に
   置かれるため、万一プレビュー内で XSS が成立しても、Tauri の ACL が別オリジンからの app command を拒否し、
   `read_file` などへ到達できない（多層防御の一枚）。
   ※同一オリジン iframe では Windows の invoke 例外（CVE-2024-35222）があり隔離しきれない。

2. **メインワールドを経由させない**
   HTML 本体が一度もメインWebViewの JS を通らない（`invoke` の戻り値に HTML を乗せない）。

3. **IPC コスト回避**
   大きなプレビュー HTML を毎回 `invoke` でシリアライズして渡すと IPC 予算に響く。
   custom protocol なら Rust から直接配信できて安い。

加えて、**CSP をレスポンスヘッダで強制**できる（文書内 `<meta>` に依存しない）という利点もある。

---

## 4. ルーティング仕様

ハンドラ（`handle_preview_request`）は URL のパスで3経路に振り分ける。

| パス | 内容 | レスポンス生成 |
|---|---|---|
| `/doc/<gen>` | サニタイズ済み HTML 本体（CSP ヘッダ付き） | `document_response` |
| `/assets/<相対>` | exe 埋め込みの同梱アセット（Mermaid/KaTeX/highlight・フォント） | `asset_response` |
| `/local/<gen>/<相対>` | 文書フォルダ配下のローカル参照（画像/CSS）を封じ込め検証して配信 | `local_resource_response` |

- `<gen>` は **世代番号**。タブ/モード/差分の切替ごとに単調増加し、直近1世代のみキャッシュに保持する
  （前モードの残留防止＋メモリ節約）。
- `/local/` は `../`・絶対パス・機密ファイルを拒否し、canonicalize + prefix 検証でシンボリックリンク脱出も弾く。
- 巨大画像/SVG は配信前に寸法・要素数を計測し、上限超過なら 413 で配信拒否（UIが固まらない暴走ガード）。

### 配信までの流れ（実際に動いている経路）

```
frontend(main.ts/preview/index.ts)   Rust(command)            別WebView          Rust(handler)
  │  invoke                            │                          │                   │
  ├─ prepare_preview ────────────────► │ サニタイズ(comrak→ammonia)│                   │
  │   (src/ipc.ts:198)                 │ CSP組立 / 世代キャッシュ  │                   │
  │ ◄── PreparedPreview{url:/doc/5,…} ─┤                          │                   │
  │                                    │                          │                   │
  ├─ show_preview ────────────────────►│ 別WebView を矩形へ配置    │                   │
  │   (src/ipc.ts:220)                 │ webview.navigate(url) ──► │ http://…/doc/5    │
  │                                    │                          ├─(custom protocol)─► handle_preview_request
  │                                    │                          │ ◄── HTML+CSP ──────┤ document_response
  │                                    │                          │ 描画               │
```

ポイント：**HTML 本体は `invoke` の戻り値に乗らない**。frontend が受け取るのは URL だけで、
別WebView がその URL を `navigate` した瞬間に custom protocol 経由で HTML が直接届く。

---

## 5. pika での実装箇所（結線済み・稼働中）

custom protocol は **登録・別WebView 生成・フロント呼び出しまで結線済みで、プレビュー表示は実際に
この経路で動いている**。中心の配信ロジックは Rust の `src-tauri/src/preview.rs` に集約され、`cargo test`
で固められている。

| ファイル | 言語 | 役割 |
|---|---|---|
| `src-tauri/src/preview.rs` | Rust | ハンドラ・ルーティング・配信・サニタイズ連携・別WebView 操作 command |
| `src-tauri/src/main.rs` | Rust | スキーム登録・state 登録・別WebView 生成・IPC 発信元ガード |
| `src/ipc.ts` | TypeScript | `prepare_preview`/`show_preview`/`hide_preview`/`set_preview_bounds` の invoke ラッパ |
| `src/preview/index.ts` | TypeScript | 表示モード×差分の占有世代管理・系統A/B 判定・URL を別WebView へ反映 |
| `src/main.ts` | TypeScript | モード切替（ソース/分割/プレビュー）に応じた show/hide/bounds の駆動 |

### `src-tauri/src/preview.rs`（配信の実体）

- `PREVIEW_SCHEME = "pika-preview"` … スキーム名
- `handle_preview_request(ctx: UriSchemeContext<'_, Wry>, req: Request<Vec<u8>>) -> Response<Vec<u8>>`
  … custom protocol の心臓（v2 形式のシグネチャ）。`ctx.app_handle().try_state::<PreviewService>()` で素材を引く
- `prepare_preview`（command）… サニタイズ → 世代キャッシュ → `http://pika-preview.localhost/doc/<gen>` を返す
- `PreviewService`（managed state）… 世代キャッシュ
- `create_preview_webview` / `show_preview` / `hide_preview` / `set_preview_bounds` … 別WebView の生成・表示・配置

### `src-tauri/src/main.rs`（電源繋ぎ・実際の登録）

```rust
tauri::Builder::default()
    // …plugin など…
    .register_uri_scheme_protocol(preview::PREVIEW_SCHEME, preview::handle_preview_request) // :98
    .invoke_handler(move |invoke| {
        // 権限ゼロ別WebView（label "preview"）からの IPC は command 実行前に全拒否     // :103
        if preview::is_blocked_invoke_origin(invoke.message.webview_ref().label()) {
            invoke.resolver.reject("プレビューWebViewからのIPCは許可されていません");
            return true;
        }
        app_invoke(invoke)
    })
    .setup(|app| {
        // …watcher / snapshot…
        app.manage(preview::PreviewService::new());                                        // :133
        // 別WebView はここでは生成しない（後述）
        Ok(())
    })
    .build(tauri::generate_context!())
    .expect("pika の起動に失敗しました");

// イベントループ稼働後に別WebView を生成する（setup 内だとデッドロックするため）           // :151-161
app.run(|app_handle, event| {
    if let tauri::RunEvent::Ready = event {
        let _ = preview::create_preview_webview(app_handle);                               // :156
    }
});
```

> なぜ `RunEvent::Ready` で生成するか：Windows/WebView2 では子WebView（`add_child`）の生成完了が
> メッセージループ経由で通知されるため、イベントループ未稼働の `setup` 内で完了を待つとデッドロックする
> （メイン窓が不可視のまま固着）。そのためループ稼働後の `Ready` に遅延させている（`main.rs:136-139`）。

### `src/ipc.ts` / `src/preview/index.ts`（フロント）

- `src/ipc.ts` … `prepare_preview`（:198）で URL・nonce・flavor・hazards を受け取り、`show_preview`（:220）で
  別WebView を矩形へ配置・ナビゲートする。HTML 本体は戻り値に乗らない。
- `src/preview/index.ts` … 表示モード（source/split/preview）×差分トグルの占有世代を直列化し、
  `previewModeForPath` で系統A（.md/.markdown/.svg）/系統B（.html/.htm）を判定。最新世代の load のみ採用して
  前モード残留を防ぐ。

---

## 6. 残タスク（リリース前ゲート）

結線そのものは完了しているが、セキュリティ境界の**実機実証**は系統C（Windows 実機 Release）で行う。

- **別WebView の権限ゼロ隔離の実証**（必達）— プレビューから `invoke`/`__TAURI_INTERNALS__` 経由の任意
  command が到達不能であることを実機で確認する（1つでも到達したら設計やり直し）。`main.rs:103` の発信元
  ガードと、capability に `preview` を含めない構成の二段で担保している前提を実測で固める。
- a11y（ナレーター/UIA が別WebView 内プレビューを辿れるか）・プレビュー内検索の実機確認。

---

## 7. 参考

- Tauri API: `https://docs.rs/tauri/latest/tauri/struct.Builder.html`
  （`register_uri_scheme_protocol` / `register_asynchronous_uri_scheme_protocol`）
- Tauri アーキテクチャ: `https://v2.tauri.app/concept/architecture/`
- Tauri セキュリティ/CSP: `https://v2.tauri.app/security/`
- 設計の正典: `docs/specs/2026-06-20-tauri-rewrite-architecture-design.md`（6章）
