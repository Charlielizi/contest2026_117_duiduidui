#!/usr/bin/env python3
import argparse
import hashlib
import pathlib
import re


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("model", type=pathlib.Path)
    ap.add_argument("output", type=pathlib.Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--labels", choices=("legacy4", "unknown-zh", "unknown-zh-mf32"), default="legacy4")
    args = ap.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.version):
        ap.error("version must contain only letters, digits, dot, underscore or hyphen")
    prefix = {"legacy4": "", "unknown-zh": "zh2-v1:",
              "unknown-zh-mf32": "zh2-mf1:"}[args.labels]
    version = prefix + args.version
    data = args.model.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    rows = []
    for offset in range(0, len(data), 12):
        rows.append("    " + ", ".join(f"0x{x:02x}" for x in data[offset:offset + 12]) + ",")
    text = "\n".join([
        '#include "voice/wakeword_model_data.h"', "",
        "alignas(16) const unsigned char g_openvela_wakeword_model[] = {",
        *rows, "};",
        f"const size_t g_openvela_wakeword_model_len = {len(data)};",
        f'const char g_openvela_wakeword_model_version[] = "{version}";',
        f'const char g_openvela_wakeword_model_sha256[] = "{digest}";', ""
    ])
    args.output.write_text(text, encoding="utf-8")
    print(digest)


if __name__ == "__main__":
    main()
