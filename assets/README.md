# Bundled splash assets

`mez.png` is the image supplied with the user's inject-animation request on
2026-10-26. It is included without modification.

SHA-256: `f364e70b55c6aebcf8234a0ef446d068bfcd73996aef58919416f818ce3bfaad`.

`back.png` (1672×941) is the Social screen background supplied with the Social
specification on 2026-10-27. It is included without modification and blurred at
runtime. SHA-256: `221f7da039f9e71e9c0550d58eed6933a0df6103f82ca80c5340e6e7ae3e0e9d`.

`cmake/EmbedFonts.cmake` pins and embeds these images together with the supplied
`fonts/kawai.otf`. The font manager decodes `mez.png` with Windows Imaging
Component and copies its RGBA pixels into a padded font-atlas rectangle after
each atlas build, so the picture shares the backend's existing font texture.
`back.png` is private to `SocialBackground.cpp`, which decodes it from memory on
a worker thread and uploads only the blurred result as a texture of its own.
