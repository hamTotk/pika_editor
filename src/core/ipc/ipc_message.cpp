#include "core/ipc/ipc_message.h"

#include "core/snapshot/json_lite.h"

namespace pika::core::ipc
{

namespace json = pika::core::snapshot::json;

bool is_absolute_windows_path(const std::string& path)
{
    // UNC: `\\server\share`（先頭が `\\` または `//`）。
    if (path.size() >= 2)
    {
        const char a = path[0];
        const char b = path[1];
        if ((a == '\\' || a == '/') && (b == '\\' || b == '/'))
        {
            return true;
        }
    }
    // ドライブ絶対: `C:\` または `C:/`（ドライブレター＋コロン＋区切り）。
    // `C:foo`（ドライブ相対）は呼び出し元 CWD 依存のため絶対と認めない（要件3.2）。
    if (path.size() >= 3)
    {
        const char drive = path[0];
        const bool is_letter = (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
        if (is_letter && path[1] == ':' && (path[2] == '\\' || path[2] == '/'))
        {
            return true;
        }
    }
    return false;
}

std::string serialize_request(const IpcRequest& req)
{
    json::Value root = json::Value::object();
    root.set("op", json::Value::str("open"));
    root.set("goto", json::Value::boolean(req.goto_mode));

    json::Value targets = json::Value::array();
    for (const OpenTarget& t : req.targets)
    {
        json::Value obj = json::Value::object();
        obj.set("path", json::Value::str(t.path));
        obj.set("line", json::Value::integer(t.line));
        obj.set("column", json::Value::integer(t.column));
        targets.push_back(std::move(obj));
    }
    root.set("targets", std::move(targets));

    // json_lite は制御文字を \uXXXX へエスケープするため、結果は改行を含まない 1 行になる。
    return root.dump();
}

namespace
{

// 受信側の唯一の受理操作はパスのオープン。op は固定で "open" のみ受理する（要件3.2）。
bool extract_target(const json::Value& obj, OpenTarget& out)
{
    if (!obj.is_object())
    {
        return false;
    }
    const json::Value* path = obj.get("path");
    if (path == nullptr || path->type() != json::Type::String)
    {
        return false;
    }
    const std::string& p = path->as_string();
    // 空・相対パスは破棄（クライアントが絶対パス化して転送する契約。要件3.2）。
    if (!is_absolute_windows_path(p))
    {
        return false;
    }
    out.path = p;

    // line/column は任意。存在する場合は整数のみ受理し、非整数なら破棄（スキーマ違反）。
    if (const json::Value* line = obj.get("line"))
    {
        if (line->type() != json::Type::Int)
        {
            return false;
        }
        const std::int64_t v = line->as_int(0);
        out.line = (v > 0 && v <= 2'000'000'000LL) ? static_cast<int>(v) : 0;
    }
    if (const json::Value* col = obj.get("column"))
    {
        if (col->type() != json::Type::Int)
        {
            return false;
        }
        const std::int64_t v = col->as_int(0);
        out.column = (v > 0 && v <= 2'000'000'000LL) ? static_cast<int>(v) : 0;
    }
    return true;
}

} // namespace

bool parse_request(const std::string& line, IpcRequest& out)
{
    // 長さ上限超過は読まずに破棄（要件3.2「最大数KBで打ち切り」）。
    if (line.size() > kMaxMessageBytes)
    {
        return false;
    }

    json::Value root;
    if (!json::parse(line, root) || !root.is_object())
    {
        return false;
    }

    // op は "open" のみ受理（受理操作をパスのオープンに限定。要件3.2）。
    const json::Value* op = root.get("op");
    if (op == nullptr || op->type() != json::Type::String || op->as_string() != "open")
    {
        return false;
    }

    const json::Value* targets = root.get("targets");
    if (targets == nullptr || !targets->is_array())
    {
        return false;
    }

    IpcRequest req;
    if (const json::Value* g = root.get("goto"))
    {
        // goto は任意フィールド。bool 以外は破棄（スキーマ違反）。
        if (g->type() != json::Type::Bool)
        {
            return false;
        }
        req.goto_mode = g->as_bool(false);
    }

    for (const json::Value& item : targets->items())
    {
        OpenTarget t;
        if (!extract_target(item, t))
        {
            // 1 件でもスキーマ違反があれば全体を破棄する（部分受理で誤動作させない）。
            return false;
        }
        req.targets.push_back(std::move(t));
    }

    // targets が空のリクエストは「前回状態の前面化」相当として受理する（パス無し転送＝要件3.1）。
    out = std::move(req);
    return true;
}

} // namespace pika::core::ipc
