# Bundled fonts

English text throughout ClickGUI and ArrayList uses the user-supplied
`dynapuff.medium.ttf` (DynaPuff Medium), supplied on 2026-10-26 and embedded
unchanged. SHA-256:
`3fd5ca3792117b8b38051498b96038497dac8fd74937d7785059f94a58882d1f`.
Copyright 2021 The DynaPuff Project Authors. The font's embedded license metadata
specifies SIL Open Font License 1.1; see [DynaPuff-OFL.txt](DynaPuff-OFL.txt).
CMake pins the bytes and copies this notice alongside the binaries.
ClickGUI preserves its existing pixel-height sizes; ArrayList preserves its
15/12 em-pixel sizes and fractional scaling. Japanese uses `kawai.otf`.

## Previous Geist assets

These unmodified static TrueType faces come from the official
[Vercel Geist repository](https://github.com/vercel/geist-font) at commit
[`be21666703155b45d8bfb1e7fe5652fe48c842cb`](https://github.com/vercel/geist-font/tree/be21666703155b45d8bfb1e7fe5652fe48c842cb).
Downloaded 2026-10-25. Both faces are SIL Open Font License 1.1; preserve
[OFL.txt](OFL.txt) with redistribution.

| File | Official source path | Weight | SHA-256 |
| --- | --- | --- | --- |
| Geist-SemiBold.ttf | `fonts/Geist/ttf/Geist-SemiBold.ttf` | 600 | `b9f52322d6d20a7ff1e7fe1310ff12ec851aebb83d24f8b4f1d4149298f489b2` |
| GeistMono-Medium.ttf | `fonts/GeistMono/ttf/GeistMono-Medium.ttf` | 500 | `6fa6a795c17a11bcbef410f52a853e8503f8e25d92762d202d907c54f5c0c0fb` |

`cmake/EmbedFonts.cmake` verifies the hashes and generates the private C++ byte
arrays at configure time for DynaPuff. The previous Geist faces remain as source
assets and are no longer embedded. Font loading never depends on installation,
external asset paths, or runtime network requests.

ArrayList name and mode sizes use 15 and 12 physical em pixels multiplied by the
ArrayList scale. The font manager converts em units into ImGui/STB pixel-height
units before rasterisation and preserves fractional requested sizes. Drawing
uses that exact built font size. Japanese text uses the supplied `kawai.otf`
fallback at a matching em size.

`kawai.otf` was supplied by the user on 2026-10-26 and is embedded unchanged.
SHA-256: `32146e718f54705267003b11df2013553c7e17d224a0fd49cca50353e81ccd87`.
The inject splash has a dedicated face containing only `メジコン`; other UI
faces merge the Japanese characters actually requested. The Geist OFL notice
above applies to the two Geist faces.
