#!/usr/bin/env python3
"""Regenerate SptagPostingLuaEmbed.h from the canonical sptag_posting.lua source.

The C++ client ships the Lua UDF to the Aerospike cluster on connect via
`aerospike_udf_put`, reading from a `constexpr const char[]` baked into the
binary. To avoid drift between the source-of-truth `.lua` file and the embedded
copy, run this script whenever the Lua changes:

    python3 SPTAG/AnnService/scripts/embed_sptag_posting_lua.py
"""
from __future__ import annotations

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LUA_SRC   = REPO_ROOT / "AnnService" / "udf" / "sptag_posting.lua"
HDR_OUT   = REPO_ROOT / "AnnService" / "inc" / "Helper" / "SptagPostingLuaEmbed.h"

# We use a raw string literal with a unique delimiter to avoid having to
# escape any character that occurs in Lua source. The delimiter must not
# appear in the Lua content itself.
DELIM = "LUA"


def main() -> None:
    lua = LUA_SRC.read_text(encoding="utf-8")
    closing = ")" + DELIM + '"'
    if closing in lua:
        raise SystemExit(
            f"Lua source contains the raw-string terminator {closing!r}; "
            "pick a different DELIM in this script."
        )

    header = (
        "#pragma once\n"
        "\n"
        "// Auto-generated from udf/sptag_posting.lua \u2014 do not edit by hand.\n"
        "// Regenerate: python3 AnnService/scripts/embed_sptag_posting_lua.py\n"
        "\n"
        "#include <cstddef>\n"
        "\n"
        "namespace SPTAG::Helper {\n"
        "\n"
        f"static constexpr const char kSptagPostingLua[] = R\"{DELIM}(\n"
        f"{lua}"
        f"){DELIM}\";\n"
        "\n"
        "static constexpr size_t kSptagPostingLuaSize = sizeof(kSptagPostingLua) - 1;\n"
        "\n"
        "} // namespace SPTAG::Helper\n"
    )
    HDR_OUT.write_text(header, encoding="utf-8")
    print(f"wrote {HDR_OUT} ({len(lua)} bytes of Lua)")


if __name__ == "__main__":
    main()
