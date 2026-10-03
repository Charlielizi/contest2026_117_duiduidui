import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";
import { fileURLToPath } from "node:url";
import { publicAssetVersion, versionPublicAssetUrls } from "./tools/public-asset-versions";

const publicDirectory = fileURLToPath(new URL("./public", import.meta.url));
const version = (url: string) => publicAssetVersion(publicDirectory, url);

export default defineConfig({
  root: "board",
  base: "/",
  publicDir: "../public",
  plugins: [react(), { name: "version-public-assets", transformIndexHtml: {
    order: "pre", handler: (html) => versionPublicAssetUrls(html, version),
  } }],
  define: { __LABTWIN_LOGO_URL__: JSON.stringify(`/assets/labtwin-logo.png?v=${version("/assets/labtwin-logo.png")}`) },
  build: {
    outDir: "../dist-board",
    emptyOutDir: true,
    assetsDir: "assets",
    sourcemap: false,
    target: "es2020",
    chunkSizeWarningLimit: 650,
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (id.includes("echarts")) return "charts";
          if (id.includes("docx") || id.includes("jszip")) return "report-export";
          return undefined;
        },
      },
    },
  },
});
