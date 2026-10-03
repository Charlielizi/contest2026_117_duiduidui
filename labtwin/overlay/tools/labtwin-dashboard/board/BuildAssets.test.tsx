import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { publicAssetDigest, publicAssetVersion, versionPublicAssetUrls } from "../tools/public-asset-versions";

describe("fixed-name public assets are content-versioned", () => {
  it("versions every extension script, stylesheet and favicon from the actual bytes", () => {
    const directory = fileURLToPath(new URL("../public", import.meta.url));
    const html = readFileSync(new URL("./index.html", import.meta.url), "utf8");
    const versioned = versionPublicAssetUrls(html, url => publicAssetVersion(directory, url));
    for (const asset of ["recorder-portal.js", "recorder-portal.css", "voice-config-portal.js", "voice-config-portal.css", "labtwin-logo.png"]) {
      expect(versioned).toContain(`/assets/${asset}?v=${publicAssetVersion(directory, `/assets/${asset}`)}`);
    }
    expect(versioned).not.toContain("v=20260915-2");
  });
  it("changes the cache key when content changes and retains module inputs", () => {
    const html = '<script src="/assets/recorder-portal.js"></script><script src="/main.tsx"></script>';
    expect(versionPublicAssetUrls(html, () => "old")).not.toBe(versionPublicAssetUrls(html, () => "new"));
    expect(versionPublicAssetUrls(html, () => "new")).toContain('src="/main.tsx"');
  });
  it("fails the build on missing or out-of-root public inputs", () => {
    const directory = fileURLToPath(new URL("../public", import.meta.url));
    expect(() => publicAssetVersion(directory, "/../private")).toThrow("escaped");
    expect(() => publicAssetVersion(directory, "/assets/not-a-file.js")).toThrow();
  });
  it("uses identical versions for Windows and Ubuntu canonical text, without changing binary bytes", () => {
    expect(publicAssetDigest(Buffer.from("script();\r\n"), true)).toBe(publicAssetDigest(Buffer.from("script();\n"), true));
    expect(publicAssetDigest(Buffer.from("png\r\n"), false)).not.toBe(publicAssetDigest(Buffer.from("png\n"), false));
  });
});
