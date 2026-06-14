// pika util: コア公開 API の戻り値型 Result<T>。
// design.md 1章「例外境界」/ 15章「コア公開APIは Result<T>。例外はモジュール内部に閉じる」。
//
// コアサービスの公開 API は例外を投げず、成功値（T）または失敗情報（ErrorInfo）を返す。
// 内部例外は境界で捕捉してログ＋エラー値化する（throw はモジュール内部に閉じる）。
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace pika::util
{

// 失敗の種別。UI 文言の出し分けと診断ログの分類に使う。
// 値は安定（永続化はしないが、テスト・ログで照合する）。
enum class ErrorCode
{
    Unknown = 0,
    InvalidArgument, // 引数・入力が不正
    NotFound,        // 対象（ファイル・エントリ）が存在しない
    Io,              // 読み書き・rename 等の I/O 失敗
    Encoding,        // デコード/エンコード（表現不能文字を含む）の失敗
    Unsupported,     // 機能としてサポート外（巨大ファイル段階など）
    Cancelled,       // 協調キャンセルで中断
};

// 失敗情報。code（分類）＋ message（人間可読・ログ向け）。
// message にファイル内容を載せない（診断ログは内容を書かない方針。design.md 12章・要件12.4）。
struct ErrorInfo
{
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
};

// 成功値 T か ErrorInfo のいずれかを保持する。例外を投げずに失敗を伝播する。
// 既定構築は禁止（必ず ok / err 経由で構築させ、未初期化の曖昧さを排除する）。
template <typename T> class Result
{
  public:
    static Result ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }

    static Result err(ErrorInfo error) { return Result(std::in_place_index<1>, std::move(error)); }

    static Result err(ErrorCode code, std::string message)
    {
        return err(ErrorInfo{code, std::move(message)});
    }

    bool is_ok() const noexcept { return slot_.index() == 0; }
    bool is_err() const noexcept { return slot_.index() == 1; }
    explicit operator bool() const noexcept { return is_ok(); }

    // value() は is_ok() のときのみ呼ぶ（呼び出し側が分岐で保証する契約）。
    const T& value() const& { return std::get<0>(slot_); }
    T& value() & { return std::get<0>(slot_); }
    T&& value() && { return std::get<0>(std::move(slot_)); }

    const ErrorInfo& error() const& { return std::get<1>(slot_); }

    ErrorCode code() const { return std::get<1>(slot_).code; }

  private:
    template <std::size_t I, typename U>
    Result(std::in_place_index_t<I> idx, U&& v) : slot_(idx, std::forward<U>(v))
    {
    }

    std::variant<T, ErrorInfo> slot_;
};

// 値を返さない（成功/失敗のみ伝える）操作のための特殊化。
template <> class Result<void>
{
  public:
    static Result ok() { return Result{}; }

    static Result err(ErrorInfo error)
    {
        Result r;
        r.error_ = std::move(error);
        r.has_error_ = true;
        return r;
    }

    static Result err(ErrorCode code, std::string message)
    {
        return err(ErrorInfo{code, std::move(message)});
    }

    bool is_ok() const noexcept { return !has_error_; }
    bool is_err() const noexcept { return has_error_; }
    explicit operator bool() const noexcept { return is_ok(); }

    const ErrorInfo& error() const& { return error_; }
    ErrorCode code() const { return error_.code; }

  private:
    Result() = default;

    ErrorInfo error_;
    bool has_error_ = false;
};

} // namespace pika::util
