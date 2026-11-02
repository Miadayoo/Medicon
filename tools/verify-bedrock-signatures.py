"""Read-only PE signature audit. Run in Python, or pass this file to IDA MCP.

No process is opened and no IDB is modified. A unique match is address evidence,
not proof of a native calling convention or object layout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        nt = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[:2] != b"MZ" or self.data[nt:nt + 4] != b"PE\0\0":
            raise ValueError("Expected a PE image")
        machine, count, self.timestamp = struct.unpack_from("<HHI", self.data, nt + 4)
        optional_size = struct.unpack_from("<H", self.data, nt + 20)[0]
        optional = nt + 24
        if machine != 0x8664 or struct.unpack_from("<H", self.data, optional)[0] != 0x20B:
            raise ValueError("Expected a Windows x64 image")
        self.base = struct.unpack_from("<Q", self.data, optional + 24)[0]
        self.size, = struct.unpack_from("<I", self.data, optional + 56)
        self.checksum, = struct.unpack_from("<I", self.data, optional + 64)
        self.sections = []
        for index in range(count):
            at = optional + optional_size + index * 40
            name = self.data[at:at + 8].rstrip(b"\0").decode("ascii")
            virtual_size, rva, size, offset = struct.unpack_from("<IIII", self.data, at + 8)
            flags, = struct.unpack_from("<I", self.data, at + 36)
            self.sections.append((name, rva, self.data[offset:offset + min(size, virtual_size)], flags))

    def read(self, rva, size):
        for _, start, data, _ in self.sections:
            if start <= rva and rva + size <= start + len(data):
                return data[rva - start:rva - start + size]
        raise ValueError(f"Unmapped RVA {rva:#x}")

    def matches(self, pattern):
        tokens = pattern.split()
        chunks = []
        for index, token in enumerate(tokens):
            if token in ("?", "??"):
                continue
            value = bytes([int(token, 16)])
            if chunks and chunks[-1][0] + len(chunks[-1][1]) == index:
                chunks[-1] = (chunks[-1][0], chunks[-1][1] + value)
            else:
                chunks.append((index, value))
        anchor_offset, anchor = max(chunks, key=lambda item: len(item[1]))
        found = []
        for _, rva, data, flags in self.sections:
            if not flags & 0x20000000:
                continue
            cursor = 0
            while True:
                position = data.find(anchor, cursor)
                if position < 0:
                    break
                cursor = position + 1
                start = position - anchor_offset
                if 0 <= start and start + len(tokens) <= len(data) and all(
                        data[start + offset:start + offset + len(chunk)] == chunk for offset, chunk in chunks):
                    found.append(rva + start)
        return found


def audit(executable, manifest, output, use_ida=False):
    image = Image(executable)
    source = json.loads(Path(manifest).read_text(encoding="utf-8"))
    results = []
    for signature in source["signatures"]:
        matches = image.matches(signature["pattern"])
        result = {"name": signature["name"], "matches": [hex(rva) for rva in matches]}
        if len(matches) == 1:
            match = matches[0]
            target = match
            if signature["resolution"] == "relative32":
                offset = signature["displacement_offset"]
                displacement, = struct.unpack("<i", image.read(match + offset, 4))
                target = match + offset + 4 + displacement
            result["status"] = "unique" if 0 <= target < image.size else "out_of_bounds"
            result["target_rva"] = hex(target)
            if use_ida:
                import ida_bytes
                import ida_funcs
                import ida_nalt
                base = ida_nalt.get_imagebase()
                if Path(ida_nalt.get_input_file_path()).resolve() != image.path.resolve():
                    raise ValueError("IDA input differs from the audited executable")
                length = len(signature["pattern"].split())
                result["ida_bytes_agree"] = ida_bytes.get_bytes(base + match, length) == image.read(match, length)
                function = ida_funcs.get_func(base + target)
                result["ida_function_start"] = hex(function.start_ea - base) if function else None
        else:
            result["status"] = "missing" if not matches else "ambiguous"
        results.append(result)
    report = {
        "executable": image.path.name,
        "sha256": hashlib.sha256(image.data).hexdigest(),
        "timestamp": hex(image.timestamp), "image_size": hex(image.size), "checksum": hex(image.checksum),
        "source": source["source"], "runtime_verified": False,
        "verification": "IDA MCP bytes and function boundaries" if use_ida else "PE bytes only",
        "signatures": results,
    }
    Path(output).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    counts = {status: sum(item["status"] == status for item in results)
              for status in ("unique", "missing", "ambiguous", "out_of_bounds")}
    print(json.dumps({"output": str(output), "counts": counts}))
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable")
    parser.add_argument("--manifest", default="data/signatures/solstice.json")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    audit(args.executable, args.manifest, args.output)
