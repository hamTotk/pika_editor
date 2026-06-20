import { defineConfig } from "vite";

// pika フロントエンド（Tauri WebView 内のアプリシェル）の Vite 設定。
// フロントの実体は src/（design doc 13章）。ビルド成果物はリポジトリルートの dist/ へ出し、
// src-tauri/tauri.conf.json の frontendDist "../dist" と一致させる。
export default defineConfig({
  root: "src",
  // Tauri はカスタムプロトコルで相対参照するため base は相対にする。
  base: "./",
  build: {
    outDir: "../dist",
    emptyOutDir: true,
    target: "es2022",
  },
  server: {
    port: 5173,
    strictPort: true,
  },
  // Tauri の開発時 HMR はデフォルトで動く。clearScreen を切ってログを潰さない。
  clearScreen: false,
});
