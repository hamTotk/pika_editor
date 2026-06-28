// パス文字列の正規化ユーティリティ（フロント単一源）。
//
// 旧来 main.ts(selfKey/samePath/parentDirOf/basename)・tree.ts(normPath)・unread.ts(normalizeSep) に
// 散らばっていた5系統を、用途別の少数関数へ集約する（S7・重複統合）。pika はパスを2用途で扱う:
//
//   1. **backend の索引キーとの突合**（未読ストア・ツリー展開集合・子キャッシュのキー）。
//      区切りだけを `/` へ寄せ **大文字小文字は保持** する。pika-core 側 normalize_path_key と突合する
//      索引キーなので大小を潰すとビュー間で未読マーク/キャッシュキーがずれる（→ pathKey）。
//   2. **同一ファイルかの等価比較**（フォルダ一致判定など）。区切り・末尾区切り・大小を吸収する
//      （→ samePathKey / samePath）。
//
// 用途を取り違えないよう関数名で固定する。挙動は各旧実装と1ミリも変えない（純粋リファクタ）。

/**
 * 索引キー正規化: 区切り `\`→`/` のみ（**大小保持**・末尾区切りも保持）。
 * backend 索引キー（未読/ツリー展開/子キャッシュ）の突合に使う。旧 unread.normalizeSep / tree.normPath と同一。
 */
export function pathKey(p: string): string {
  return p.replace(/\\/g, "/");
}

/**
 * 等価比較キー: 区切り `\`→`/`・末尾区切り除去・小文字化。同一パス判定（samePath）の内部キー。
 * 旧 main.ts samePath 内の `norm` と同一。
 */
export function samePathKey(p: string): string {
  return p.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();
}

/** 2つのパスが同一を指すか（区切り/末尾区切り/大小を吸収して比較）。旧 main.ts samePath と同一。 */
export function samePath(a: string, b: string): boolean {
  return samePathKey(a) === samePathKey(b);
}

/** パスの末尾要素（ファイル/フォルダ名）。区切りが無ければ元の文字列を返す。旧 `p.split(/[\\/]/).pop() ?? p` と同一。 */
export function basename(p: string): string {
  return p.split(/[\\/]/).pop() ?? p;
}

/** パスの親フォルダ（末尾区切り＋末尾要素を落とす）。区切りが無ければ自身を返す。旧 main.ts parentDirOf と同一。 */
export function parentDir(p: string): string {
  const m = p.replace(/[\\/]+$/, "").replace(/[\\/][^\\/]+$/, "");
  return m || p;
}
