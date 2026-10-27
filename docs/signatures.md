# Solstice signatures

Medicon uses pattern data from
[`surukotonai/solstice`](https://github.com/surukotonai/solstice), pinned to
[`1d7808f52ef4ba6e34a7fbcc65294907a7761611`](https://github.com/surukotonai/solstice/commit/1d7808f52ef4ba6e34a7fbcc65294907a7761611).
The import contains the 63 active `DEFINE_SIG` entries in
[`src/SDK/SigManager.hpp`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/SDK/SigManager.hpp).
Commented-out entries and the upstream scanner/SDK implementation are excluded.

`data/signatures/solstice.json` records each pattern, resolution kind, displacement
offset, upstream source line, version comment when present, and whether the
upstream comment expresses uncertainty. `src/memory/solstice_catalog.inc` embeds
the same data in the library, so loading the client never depends on a working
directory or network access.

The upstream revision has no root `LICENSE`, `LICENCE`, or `COPYING` file. Bundled
dependency licenses do not grant a license to `SigManager.hpp`. The manifest
records `no_root_license_found`; no upstream license grant is asserted here.

## Compatibility

The source contains mixed version comments, including `26.51`, `1.26.0`, and
`1.21.51`. They are upstream hints, not a supported-version declaration. This
foundation has not been verified against a running Minecraft build. A unique
byte match also does not establish a function's calling convention or ABI.
The foundation provides address discovery and diagnostics; the host integration
must verify the installed game version and ABI before installing hooks.

Every scan covers explicit readable memory regions. `find_unique` withholds a
usable result when there are zero or multiple matches. Relative references read
only a bounded signed little-endian 32-bit displacement and require the resulting
address to lie inside the allowed image range. Missing, malformed, ambiguous,
duplicate-name, and out-of-bounds entries have diagnostic statuses and no address.

`current_process_module` inspects the Windows x64 executable already loaded in
the current process. It checks PE headers and bounds, selects executable PE
sections, and excludes uncommitted, guarded, unreadable, and non-executable pages.
It does not open another process, inject a DLL, or write memory. Memory views
must not outlive the loaded image; a host must coordinate scanning with changes
to page protections or module lifetime.

```cpp
#include <medicon/memory/module_view.hpp>
#include <medicon/memory/signature_registry.hpp>

medicon::memory::SignatureRegistry signatures;
std::string error;
if (auto module = medicon::memory::current_process_module(&error)) {
    signatures.scan(medicon::memory::solstice_signatures(),
                    module->executable_regions, module->image);
    // Optional address: check availability before using any integration.
    const auto render = signatures.address("ScreenView_setupAndRender");
}
```

The registry is mutable during initialization and should be treated as immutable
while consumers use it. Rescanning replaces earlier results. A scanner only
searches within each supplied region and does not combine disconnected regions.

## Re-import

With a local checkout at `.reference/solstice`:

```powershell
./tools/import-solstice-signatures.ps1
```

The default commit is pinned; local checkout changes do not alter the import.
To intentionally review a newer revision, first fetch it into that checkout,
then run the importer with `-Commit <full-commit>`. The script reads the committed
file with `git show`, validates tokens, rejects duplicate names or unsupported
direct offsets, and regenerates both artifacts. Review pattern and provenance
changes together, update the pinned count/commit checks in
`tests/signature_tests.cpp`, and test the actual target Minecraft version before
enabling integrations. The script performs no network operations.
