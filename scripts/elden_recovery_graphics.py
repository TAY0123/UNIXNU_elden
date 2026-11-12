# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive Elden's 4 KiB display rules from the paired 25F71 policies."""

import hashlib
import json
from pathlib import Path


POLICIES = {
    "native-no-metal-scaled-plan-40270321747",
    "native-bgra-framebuffer",
    "software-pool-bgra",
    "software-no-display-compression",
    "software-vsync-off-uncompressed",
}

ELDEN_POLICY = Path(__file__).resolve().parents[1] / "configs/elden-macos26-display-pages.json"


def render(manifest: Path, cache: Path, page_bytes: int) -> str:
    from native_recovery_ui import PAGE, validate, verify_page

    if cache.name != "dyld_shared_cache_arm64e.01" or page_bytes != 4096:
        raise ValueError("Elden graphics requires the 25F71 ARM64e .01 cache and 4 KiB pages")

    def initializer(data: bytes) -> str:
        return "{ " + ", ".join(f"0x{byte:02x}" for byte in data) + " }"

    def digest(data: bytes) -> str:
        return initializer(hashlib.sha256(data).digest())

    document = json.loads(manifest.read_text())
    document["pages"].extend(json.loads(ELDEN_POLICY.read_text())["pages"])
    rules = validate(document)
    rows = ["// Generated from the verified 25F71 graphics rules.",
            "#pragma once", "static const struct sheng_ui_page sheng_ui_pages[] = {"]
    seen = set()
    offsets = set()
    with cache.open("rb") as stream:
        for rule in rules:
            edits = [edit for edit in rule["edits"] if edit.get("policy") in POLICIES]
            if not edits:
                continue
            if rule["file"] != cache.name or len(edits) != 1 or len(rule["edits"]) != 1:
                raise ValueError("expected one graphics edit in a dedicated reference page")
            edit = edits[0]
            policy = edit["policy"]
            before = bytes.fromhex(edit["before"])
            after = bytes.fromhex(edit["after"])
            start, offset = divmod(edit["offset"], page_bytes)
            start *= page_bytes
            file_offset = rule["file_offset"] + start
            if policy in seen or file_offset in offsets or offset + len(before) > page_bytes:
                raise ValueError("duplicate graphics rule or edit spans a 4 KiB page")
            stream.seek(rule["file_offset"])
            original = stream.read(PAGE)
            updated = verify_page(original, rule)
            rows.extend([
                "    {",
                f'        .file = "{cache.name}",',
                f"        .file_offset = {file_offset}ULL,",
                f"        .before_sha256 = {digest(original[start:start + page_bytes])},",
                f"        .after_sha256 = {digest(updated[start:start + page_bytes])},",
                "        .edit_count = 1,",
                "        .edits = {{",
                f"            .offset = {offset}, .size = {len(before)},",
                f"            .before = {initializer(before)}, .after = {initializer(after)},",
                "        }},",
                "    },",
            ])
            seen.add(policy)
            offsets.add(file_offset)
    if seen != POLICIES:
        raise ValueError("the manifests are missing an Elden graphics rule")
    return "\n".join((*rows, "};", ""))
