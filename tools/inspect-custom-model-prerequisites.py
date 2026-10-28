"""Read-only, bounded prerequisite audit; never installs hooks or infers an ABI."""

import argparse
import bisect
import hashlib
import json
import mmap
from pathlib import Path
import re
import struct
import subprocess


PATTERNS = (
    "ActorRenderDispatcher_render",
    "ItemRenderer_render",
    "ActorShaderManager_setupShaderParameters",
    "LevelRenderer_renderLevel",
    "Unknown_renderNametag",
)
ANCHORS = (
    "controller.render.player", "rightItem", "textures/models/armor/",
    "entity_alphatest", ".?AVPlayerRenderer@@", ".?AVActorRenderDispatcher@@",
    "Tessellator", "renderMeshImmediately",
)


def audit(exe, solstice):
    signature_file = solstice / "src/SDK/SigManager.hpp"
    source = signature_file.read_text(encoding="utf-8-sig")
    revision = subprocess.run(
        ["git", "-C", str(solstice), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    # Detect local source edits so the recorded revision is not misleading.
    source_status = subprocess.run(
        ["git", "-C", str(solstice), "status", "--porcelain", "--", "src/SDK/SigManager.hpp"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    report = {
        "scope": "Read-only PE pattern/anchor scan. Xrefs are byte candidates only; no ABI or render coverage verified.",
        "upstream": {"repository": "https://github.com/surukotonai/solstice", "commit": revision,
                     "signature_file": "src/SDK/SigManager.hpp", "source_modified": bool(source_status)},
        "binary": {"path": str(exe)}, "patterns": {}, "anchors": {},
    }
    with exe.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
        if image[:2] != b"MZ":
            raise ValueError("Expected a PE executable")
        pe = struct.unpack_from("<I", image, 0x3C)[0]
        if image[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", image, pe + 4)[0] != 0x8664:
            raise ValueError("Expected Windows x64 PE")
        optional = pe + 24
        if struct.unpack_from("<H", image, optional)[0] != 0x20B:
            raise ValueError("Expected PE32+")
        count = struct.unpack_from("<H", image, pe + 6)[0]
        optsize = struct.unpack_from("<H", image, pe + 20)[0]
        sections = []
        for index in range(count):
            offset = optional + optsize + index * 40
            _, rva, size, raw = struct.unpack_from("<IIII", image, offset + 8)
            if raw + size > len(image):
                raise ValueError("Truncated section")
            flags = struct.unpack_from("<I", image, offset + 36)[0]
            sections.append((rva, raw, size, bool(flags & 0x20000000)))
        report["binary"].update(
            sha256=hashlib.sha256(image).hexdigest(),
            image_size=hex(struct.unpack_from("<I", image, optional + 56)[0]),
            timestamp=hex(struct.unpack_from("<I", image, pe + 8)[0]),
        )

        def file_rva(offset):
            for rva, raw, size, _ in sections:
                if raw <= offset < raw + size:
                    return rva + offset - raw
            return None

        # PE unwind ranges only annotate potential referring functions. They do
        # not prove that a byte sequence is an instruction or a render entry.
        exception_rva, exception_size = struct.unpack_from("<II", image, optional + 112 + 3 * 8)
        ranges = []
        for rva, raw, size, _ in sections:
            if rva <= exception_rva and exception_rva + exception_size <= rva + size:
                start = raw + exception_rva - rva
                ranges = [struct.unpack_from("<III", image, start + index)
                          for index in range(0, exception_size - 11, 12)]
                ranges.sort()
                break
        starts = [entry[0] for entry in ranges]

        for name in PATTERNS:
            definition = re.search(r"(?m)^(\s*)(//\s*)?DEFINE_SIG\(" + re.escape(name)
                                   + r',\s*"([^"]+)"', source)
            if definition is None:
                report["patterns"][name] = {"status": "missing upstream"}
                continue
            pattern = definition.group(3)
            regex = re.compile(b"".join(b"." if token == "?" else re.escape(bytes([int(token, 16)]))
                                        for token in pattern.split()), re.DOTALL)
            matches = []
            for rva, raw, size, executable in sections:
                if executable:
                    matches.extend(hex(rva + match.start() - raw)
                                   for match in regex.finditer(image, raw, raw + size))
            report["patterns"][name] = {
                "pattern": pattern, "matches": matches,
                "line": source.count("\n", 0, definition.start()) + 1,
                "active_upstream": definition.group(2) is None, "abi_validated": False,
            }

        targets = {}
        for anchor in ANCHORS:
            positions = []
            position = 0
            needle = anchor.encode("ascii")
            while (position := image.find(needle, position)) >= 0:
                rva = file_rva(position)
                if rva is not None:
                    positions.append(hex(rva))
                    targets.setdefault(rva, []).append(anchor)
                position += len(needle)
            report["anchors"][anchor] = {"rvas": positions, "possible_lea_xrefs": []}

        # Limit inspection to RIP-relative LEA references to the named anchors.
        lea = re.compile(rb"[\x48-\x4f]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]....", re.DOTALL)
        for rva, raw, size, executable in sections:
            if not executable:
                continue
            for match in lea.finditer(image, raw, raw + size):
                instruction_rva = rva + match.start() - raw
                target = instruction_rva + 7 + struct.unpack_from("<i", image, match.start() + 3)[0]
                if target not in targets:
                    continue
                function = None
                index = bisect.bisect_right(starts, instruction_rva) - 1
                if index >= 0 and instruction_rva < ranges[index][1]:
                    function = hex(ranges[index][0])
                for anchor in targets[target]:
                    report["anchors"][anchor]["possible_lea_xrefs"].append(
                        {"instruction_rva": hex(instruction_rva), "unwind_range_start": function})
    return report


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--solstice", type=Path, default=root / ".reference/solstice")
    parser.add_argument("--output", type=Path, default=root / "out/custom-model/prerequisites.json")
    args = parser.parse_args()
    report = audit(args.exe, args.solstice)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for name, entry in report["patterns"].items():
        print(f"{name}: {entry.get('matches', [])}; ABI unverified")
    print(f"Report: {args.output}")


if __name__ == "__main__":
    main()
