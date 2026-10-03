import { Document, HeadingLevel, Packer, Paragraph, TextRun } from "docx";
import JSZip from "jszip";
import type { ReportBundle } from "../src/report/report";
import { sha256Hex } from "../src/crypto/portable";

function download(filename: string, blob: Blob) {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export async function downloadDocx(report: ReportBundle, experimentId: string) {
  const children = report.markdown.split("\n").map((line) => {
    if (line.startsWith("# ")) {
      return new Paragraph({ text: line.slice(2), heading: HeadingLevel.TITLE });
    }
    if (line.startsWith("## ")) {
      return new Paragraph({ text: line.slice(3), heading: HeadingLevel.HEADING_1 });
    }
    if (line.startsWith("- ")) {
      return new Paragraph({ text: line.slice(2), bullet: { level: 0 } });
    }
    return new Paragraph({ children: [new TextRun(line)] });
  });
  const document = new Document({ sections: [{ children }] });
  download(`LabTwin-${experimentId}-report.docx`, await Packer.toBlob(document));
}

export async function downloadEvidenceZip(report: ReportBundle, experimentId: string) {
  const zip = new JSZip();
  const evidence = JSON.stringify(report.evidence, null, 2);
  const events = report.evidence.events.map((event) => JSON.stringify(event)).join("\n");
  const environment = JSON.stringify(report.evidence.environment_events, null, 2);
  const manifest = JSON.stringify(
    {
      schema_version: 1,
      experiment_id: experimentId,
      generated_at: report.evidence.generated_at,
      evidence_sha256: report.digest,
      integrity: report.evidence.integrity,
      files: ["report.md", "evidence.json", "events.jsonl", "environment.json"],
    },
    null,
    2,
  );

  const files: Record<string, string> = {
    "manifest.json": manifest,
    "report.md": report.markdown,
    "evidence.json": evidence,
    "events.jsonl": events ? `${events}\n` : "",
    "environment.json": environment,
  };
  const sums = await Promise.all(
    Object.entries(files).map(async ([name, contents]) => `${sha256Hex(contents)}  ${name}`),
  );
  for (const [name, contents] of Object.entries(files)) zip.file(name, contents);
  zip.file("SHA256SUMS", `${sums.join("\n")}\n`);
  download(
    `LabTwin-${experimentId}-evidence.zip`,
    await zip.generateAsync({ type: "blob", compression: "DEFLATE" }),
  );
}
