#!/usr/bin/env node
// PostToolUse(Edit|Write): 編集されたファイルを拡張子別に検査する。
//   失敗      = exit 2 + stderr（Claude に差し戻す）
//   検査不能  = exit 0（編集はブロックしない。壊れた hook が全編集を止めないため）
// ここに入れるのは高速・ファイル単位の検査だけ。プロジェクト全体を見る検査
// （cmake ビルド / ctest）は検証コマンド側に置く（CLAUDE.md「完了の検証」参照）。
import { execFileSync } from "node:child_process";
import { readFileSync, existsSync } from "node:fs";

function passthrough() {
  process.exit(0);
}

let input;
try {
  input = JSON.parse(readFileSync(0, "utf8"));
} catch {
  passthrough();
}

const file = input?.tool_input?.file_path;
if (!file || !existsSync(file)) passthrough();

const ext = (file.split(".").pop() || "").toLowerCase();
const errors = [];

// --- C++: clang-format の整形チェック（.clang-format を基準・書き換えはしない） ---
//   hook 内で自動 format すると直前の Edit 内容とファイル実体が乖離し後続編集が壊れる。
//   よって --dry-run でチェックのみ、差分があれば差し戻して Claude に直させる。
const CPP_EXT = new Set(["cpp", "cc", "cxx", "h", "hpp", "hh"]);
if (CPP_EXT.has(ext)) {
  try {
    execFileSync("clang-format", ["--dry-run", "--Werror", file], {
      stdio: ["ignore", "pipe", "pipe"],
      timeout: 15000,
    });
  } catch (e) {
    const stderr = String(e.stderr || "");
    // 未導入（ENOENT / not recognized）は素通し。整形差分のみ差し戻す。
    const notInstalled = e.code === "ENOENT" || /not found|not recognized/i.test(stderr);
    if (!notInstalled) {
      errors.push(
        `$ clang-format --dry-run --Werror\n${[e.stdout, e.stderr].filter(Boolean).join("\n")}`
      );
    }
  }
}

// --- JSON: 構文 parse（in-process。Windows パスの引用問題を避けるため node -e は使わない） ---
if (ext === "json") {
  try {
    JSON.parse(readFileSync(file, "utf8"));
  } catch (e) {
    errors.push(`JSON 構文エラー（${file}）:\n${e.message}`);
  }
}

if (errors.length) {
  // stderr は先頭 30 行に切る（長い出力で context を圧迫しない）
  process.stderr.write(errors.join("\n").split("\n").slice(0, 30).join("\n") + "\n");
  process.exit(2);
}
process.exit(0);
