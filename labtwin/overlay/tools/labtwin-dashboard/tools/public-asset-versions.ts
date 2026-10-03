import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { extname, resolve, sep } from "node:path";

export function publicAssetDigest(bytes: Buffer, text: boolean) {
  // Match the ROMFS synchronizer's canonical LF text bytes on both hosts.
  const content = text ? bytes.toString("utf8").replace(/\r\r\n|\r\n/g, "\n") : bytes;
  return createHash("sha256").update(content).digest("hex").slice(0, 16);
}

export function publicAssetVersion(publicDirectory: string, url: string) {
  const root = resolve(publicDirectory);
  const file = resolve(root, url.replace(/^\//, ""));
  if (!file.startsWith(root + sep)) throw new Error("Public asset escaped its root");
  return publicAssetDigest(readFileSync(file), [".js", ".css", ".html"].includes(extname(file)));
}

export function versionPublicAssetUrls(html: string, version: (url: string) => string) {
  return html.replace(/((?:src|href)=["'])(\/assets\/[A-Za-z0-9._/-]+)(?:\?[^"']*)?(["'])/g,
    (_, attribute: string, url: string, quote: string) => `${attribute}${url}?v=${version(url)}${quote}`);
}
