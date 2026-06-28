// 未読状態ストア（要件4.2・ui-design 5章）。外部変更（fs-changed）を受けて
// ファイルごとの未読種別を保持し、ツリー/タブの状態マーク（± 変更 / ◆ 新規 / 取消線 削除）へ写す。
//
// フォルダの伝播マーク（子孫に未読があるフォルダにも淡い ± を付ける＝要件4.2）と
// 確認済み（8章・sprint 3）での解除はここを単一源にして両ビュー（ツリー/タブ）で共有する。

import type { FsChange } from "../ipc";
import { pathKey } from "../util/path";

/** ファイル自身の未読種別（重畳の素・ui-design 5章の状態記号に対応）。 */
export type UnreadKind = "modified" | "created" | "removed";

/** 状態記号（色だけに依存しない弁別。要件11.5/ui-design 9章）。 */
export const UNREAD_MARK: Record<UnreadKind, string> = {
  modified: "±", // 変更
  created: "◆", // 新規
  removed: "×", // 削除済み（表示は取り消し線も併用する）
};

// パス区切りの正規化（Windows の \ と / を吸収・**大小保持**）は util/path.pathKey を使う
// （backend 索引キー突合のため大小は潰さない）。

/** パスの祖先フォルダを末端から順に列挙する（伝播マーク用）。 */
function ancestors(path: string): string[] {
  const norm = pathKey(path);
  const out: string[] = [];
  let idx = norm.lastIndexOf("/");
  while (idx > 0) {
    out.push(norm.slice(0, idx));
    idx = norm.lastIndexOf("/", idx - 1);
  }
  return out;
}

/** 未読の集中管理。fs-changed を適用し、各パスの未読種別を引ける。 */
export class UnreadStore {
  /** ファイルパス（正規化済み）→ 未読種別。 */
  private files = new Map<string, UnreadKind>();
  /** 伝播用: フォルダパス（正規化済み）→ そのフォルダ配下の未読ファイル数。 */
  private folderCounts = new Map<string, number>();

  /** 合成済み外部変更を適用する（要件4.2）。 */
  apply(changes: FsChange[]): void {
    for (const c of changes) {
      switch (c.kind) {
        case "created":
          this.setFile(c.path, "created");
          break;
        case "modified":
          this.setFile(c.path, "modified");
          break;
        case "removed":
          this.setFile(c.path, "removed");
          break;
        case "renamed":
          // 旧パスの未読を新パスへ引き継ぐ（要件4.2 の継承）。
          if (c.from) {
            const prev = this.files.get(pathKey(c.from)) ?? "modified";
            this.clearFile(c.from);
            this.setFile(c.path, prev);
          } else {
            this.setFile(c.path, "modified");
          }
          break;
      }
    }
  }

  /** ファイルの未読種別を引く（無ければ undefined）。 */
  get(path: string): UnreadKind | undefined {
    return this.files.get(pathKey(path));
  }

  /** フォルダ配下に未読があるか（伝播マーク用・折りたたみ中でも気づける＝要件4.2）。 */
  folderHasUnread(folderPath: string): boolean {
    return (this.folderCounts.get(pathKey(folderPath)) ?? 0) > 0;
  }

  /** 確認済み（8章）でファイルの未読を解除する。 */
  clearFile(path: string): void {
    const norm = pathKey(path);
    if (!this.files.has(norm)) return;
    this.files.delete(norm);
    for (const anc of ancestors(norm)) {
      const n = (this.folderCounts.get(anc) ?? 0) - 1;
      if (n <= 0) this.folderCounts.delete(anc);
      else this.folderCounts.set(anc, n);
    }
  }

  /** 未読ファイル総数（ステータス表示・テスト用）。 */
  unreadCount(): number {
    return this.files.size;
  }

  /**
   * 確認済み対象になる未読ファイルのパス一覧（要件8.3「すべて確認済み」）。
   * 削除済み（removed）は確認対象外（差分・ベースラインの確定対象でない）。
   * 「実行開始時点の未読集合をフリーズ」する用途のためスナップショットを配列で返す。
   */
  confirmTargets(): string[] {
    const out: string[] = [];
    for (const [path, kind] of this.files) {
      if (kind !== "removed") out.push(path);
    }
    return out;
  }

  private setFile(path: string, kind: UnreadKind): void {
    const norm = pathKey(path);
    const existed = this.files.has(norm);
    this.files.set(norm, kind);
    if (!existed) {
      for (const anc of ancestors(norm)) {
        this.folderCounts.set(anc, (this.folderCounts.get(anc) ?? 0) + 1);
      }
    }
  }
}
