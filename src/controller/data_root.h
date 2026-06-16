// controller/data_root: データルート解決（portable.txt 検出による分岐）。
// design.md 5.1 手順1・7章 K1「データルートの解決は app が起動最初期に1回だけ行い、全モジュールへ
// 確定パスを渡す」/ 要件13章「ポータブル版は exe 隣の ./pika-data/」/ spec.md sprint2 must。
//
// wx・Win32・FS を一切含まない純ロジック。実 FS の存在判定（portable.txt の有無・環境変数取得）は
// 呼び出し側が注入する DataRootProbe
// で行い、本体はその入力から確定データルートを決定論的に組み立てる （gtest
// で両分岐を観測）。これにより「どこをデータルートにするか」の方針が自動回帰網に乗り、 UI
// 実機なしで検証できる。
#pragma once

#include <string>

namespace pika::controller
{

// データルート解決の入力（プラットフォーム層が実環境から集めて渡す）。
// portable.txt の有無は FS アクセスのため呼び出し側が判定し bool で渡す（コアを FS 非依存に保つ）。
struct DataRootProbe
{
    // exe 隣に portable.txt が存在するか（ポータブル版判定。要件13章）。
    bool portable_marker_present = false;
    // exe が置かれているディレクトリの絶対パス（末尾区切りなし）。
    // ポータブル版のとき ./pika-data/ の親になる。
    std::string exe_dir;
    // %LOCALAPPDATA%（通常版のデータルート親。例 C:\Users\<user>\AppData\Local）。
    // 末尾区切りなし。取得不能なら空文字（その場合は解決失敗）。
    std::string local_app_data;
};

// データルートの種別（どちらの分岐で解決されたかを観測可能にする）。
enum class DataRootKind
{
    // %LOCALAPPDATA%\pika\（通常インストール版）。
    LocalAppData,
    // exe 隣の ./pika-data/（ポータブル版。portable.txt 検出）。
    Portable,
};

// データルート解決の結果。resolved=false なら必要な環境情報が欠落しデータルートを確定できない
// （例: portable でないのに %LOCALAPPDATA% が取れない）。呼び出し側は退避先がないため起動を
// 中断する（データを失わない最上位原則。退避先未確定で書き込みを始めない）。
struct DataRoot
{
    bool resolved = false;
    DataRootKind kind = DataRootKind::LocalAppData;
    // 確定したデータルートの絶対パス（末尾区切りなし・'\\' 区切り）。
    std::string path;
};

// DataRootProbe からデータルートを解決する純粋関数。
//   - portable_marker_present=true: <exe_dir>\pika-data を採用（Portable）。exe_dir が空なら失敗。
//   - それ以外: <local_app_data>\pika を採用（LocalAppData）。local_app_data が空なら失敗。
// 区切りは Windows のバックスラッシュ '\\' に統一し、入力末尾の余分な区切りは畳む。
DataRoot resolve_data_root(const DataRootProbe& probe);

} // namespace pika::controller
