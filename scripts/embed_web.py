Import("env")
from pathlib import Path

WEB_DIR = Path(env.subst("$PROJECT_DIR")) / "web"
OUT_CPP = Path(env.subst("$PROJECT_DIR")) / "src" / "web_assets.cpp"
OUT_H = Path(env.subst("$PROJECT_DIR")) / "src" / "web_assets.h"

ASSETS = [
    ("login.html", "login_html"),
    ("index.html", "index_html"),
    ("image.html", "image_html"),
    ("overlay.html", "overlay_html"),
    ("network.html", "network_html"),
    ("uploads.html", "uploads_html"),
    ("system.html", "system_html"),
    ("app.css", "app_css"),
    ("app.js", "app_js"),
    ("exposure_auto.js", "exposure_auto_js"),
    ("diag.js", "diag_js"),
]


def c_symbol(name: str) -> str:
    return name.replace(".", "_").replace("-", "_")


def emit_asset(path: Path, symbol: str) -> list[str]:
    data = path.read_bytes()
    lines = [
        f"alignas(4) const uint8_t {symbol}[] = {{",
    ]
    row = "  "
    for i, byte in enumerate(data):
        row += f"0x{byte:02x}, "
        if (i + 1) % 12 == 0:
            lines.append(row.rstrip())
            row = "  "
    if row.strip():
        lines.append(row.rstrip().rstrip(","))
    lines.append("};")
    lines.append(f"const size_t {symbol}_len = {len(data)};")
    lines.append("")
    return lines


def build_assets():
    entries = []
    body = [
        '#include "web_assets.h"',
        "",
        "namespace {",
        "",
    ]
    for filename, symbol in ASSETS:
        path = WEB_DIR / filename
        if not path.exists():
            raise FileNotFoundError(f"Missing web asset: {path}")
        body.extend(emit_asset(path, symbol))
        entries.append(
            f'  {{"/{filename}", "{guess_type(filename)}", {symbol}, {symbol}_len}},'
        )
    body.extend(
        [
            "}  // namespace",
            "",
            "static const WebAsset kAssets[] = {",
            *entries,
            "};",
            "",
            "const WebAsset *webFindAsset(const char *path) {",
            "  for (const auto &asset : kAssets) {",
            "    if (strcmp(asset.path, path) == 0) return &asset;",
            "  }",
            "  return nullptr;",
            "}",
            "",
        ]
    )
    header = """#pragma once

#include <cstring>
#include <cstddef>
#include <cstdint>

struct WebAsset {
  const char *path;
  const char *content_type;
  const uint8_t *data;
  size_t size;
};

const WebAsset *webFindAsset(const char *path);
"""
    OUT_H.write_text(header, encoding="utf-8")
    OUT_CPP.write_text("\n".join(body), encoding="utf-8")


def guess_type(filename: str) -> str:
    if filename.endswith(".css"):
        return "text/css"
    if filename.endswith(".js"):
        return "application/javascript"
    return "text/html"


build_assets()
