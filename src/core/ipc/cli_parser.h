// core/ipc: CLI 引数の解釈（エージェント向けAPI）。
// 要件3章「CLI仕様」/ design.md 84行「CLI引数解析（-g 行/桁パース）」「ウィンドウ生成前の引数検証の
// 同期実行」/ spec.md sprint7。
//
// ここは純ロジック（FS・Win32・wx を一切含まない）。実在判定は呼び出し側が注入する述語
// （PathProbe）で行い、本体はその分類結果と終了コードだけを決定論的に返す（gtest で検証）。
// 「ウィンドウ生成・転送の前に検証を同期実行し、失敗時は GUI を起動せず非0で即終了」（要件3.2）の
// 判定本体をここに置く。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace pika::core::ipc
{

// -g の位置指定（VS Code 互換 <file>:<行>[:<桁>]）。
// 行・桁は 1 始まり。桁省略時は行頭（col = 1）を表す。位置指定が無いときは has_position=false。
struct CursorTarget
{
    // -g で行（任意で桁）が剥がせたか。
    bool has_position = false;
    // 1 始まりの行（has_position=true のときのみ意味を持つ）。
    int line = 0;
    // 1 始まりの桁。桁省略時は 1（行頭）。
    int column = 1;
};

// `-g <file>:<行>[:<桁>]` のパス末尾から位置指定を剥がす。
// 規則（要件3.1）:
//   - 末尾から `:<整数>`（さらに任意でもう 1 段）を桁・行として剥がし、残りをパスとする
//   - 先頭のドライブレター直後のコロン（`C:\...` の 2 文字目）は分割対象にしない
//   - 桁省略時は行頭（column=1）
//   - 行・桁が整数でない場合は位置指定を無視し、入力全体をパスとして解釈する
// 返り値の path は剥がした後のパス（位置指定が無ければ入力そのまま）。
struct GotoParse
{
    std::string path;
    CursorTarget target;
};
GotoParse parse_goto(const std::string& spec);

// 1 つのパス引数の分類（存在判定後）。要件3.2「存在しないフォルダはエラー、存在しないファイルパスは
// 新規タブ扱い」。フォルダは「単一フォルダ方針」（要件3.1）のため複数指定はエラーになる。
enum class ArgKind
{
    // 実在ファイル → タブで開く。
    ExistingFile,
    // 実在フォルダ → ツリーで開く（複数指定はエラー）。
    ExistingFolder,
    // 実在しないが、新規タブ扱いとして受理するパス。
    NewFile,
    // 末尾が区切り等で「フォルダ意図」かつ実在しない → エラー。
    MissingFolder,
};

// 呼び出し側が注入する実在判定。FS アクセスをコアから排除し、テストで決定論化するための述語。
// is_dir: パスが実在しディレクトリなら true。is_file: 実在し通常ファイルなら true。
struct PathProbe
{
    std::function<bool(const std::string&)> is_dir;
    std::function<bool(const std::string&)> is_file;
};

// 分類済みの 1 引数。target は -g 由来のカーソル位置（無ければ has_position=false）。
struct ClassifiedArg
{
    std::string path;
    ArgKind kind = ArgKind::NewFile;
    CursorTarget target;
};

// 引数検証の終了コード規約（要件3.2「0＝受理、非0＝エラー」）。
// value は実際にプロセスが返すべき終了コード（0 / 非0）。
enum class ExitCode : int
{
    // 受理（GUI 起動 or 転送へ進む）。
    Accepted = 0,
    // 不正な引数（存在しないフォルダ・複数フォルダ等）。
    InvalidArgument = 2,
};

// 検証結果。accepted=true なら GUI 生成/転送へ進む。
// false なら GUI を起動せず exit_code で終了する。args は受理時の分類済み引数列。
// message は失敗理由（診断ログ向け・内容は書かない）。
struct ValidationResult
{
    bool accepted = false;
    ExitCode exit_code = ExitCode::InvalidArgument;
    std::vector<ClassifiedArg> args;
    std::string message;
};

// 解析済みの CLI 入力（プラットフォーム層が argv→ここへ落とし込む前の中間表現）。
// goto_mode=true は `-g` 指定（要件3.1）。paths は -g のときは 1 件（spec）、通常は 0 件以上。
struct CliInvocation
{
    bool show_help = false;
    bool show_version = false;
    // -g 指定。
    bool goto_mode = false;
    // 解釈対象のパス（-g のときは唯一の spec を 1 件）。
    std::vector<std::string> paths;
};

// argv（プログラム名を除いた引数列）を CliInvocation に解釈する。
// --help/-h・--version は最優先で拾い、-g は直後の 1 引数を spec として扱う。
// 認識できないオプション（先頭 '-' で既知でないもの）はパスではなく
// 不正引数として errored=true にする。
struct ParseResult
{
    CliInvocation invocation;
    // 認識できないオプション等。
    bool errored = false;
    std::string message;
};
ParseResult parse_argv(const std::vector<std::string>& argv);

// 解析済み入力を実在判定で検証し、終了コードと分類を確定する（GUI 生成前に同期実行する本体）。
// 規則:
//   - 存在しないフォルダ意図（末尾区切り or 既存フォルダと衝突しない非ファイル）はエラー（非0）
//   - 実在フォルダは 1 つまで（複数フォルダ指定はエラー。要件3.1「単一フォルダ方針」）
//   - 実在しないファイルパスは NewFile として受理（要件3.2）
//   - 引数なし（paths 空・help/version でない）は「前回状態を復元」の受理（要件3.1）
ValidationResult validate(const CliInvocation& inv, const PathProbe& probe);

} // namespace pika::core::ipc
