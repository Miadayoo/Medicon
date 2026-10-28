# Bundled Neon model

Source: the user's `C:/Users/hina/Downloads/neon-t-pose` (Sprinter FBX and textures),
staged for the existing CustomModel importer. `model.json` retains scale 1 and
yawOffset 180. The model and three diffuse PNGs are copied unchanged.

`cmake/EmbedModel.cmake` pins the SHA-256 of these five files and adds them to
the Core DLL as Windows RCDATA resources. No Downloads path or adjacent model
directory is required in a loader installation. Normal/roughness/metallic maps
are not bundled because the current native entity renderer uses only diffuse
textures. Generated `.mmdl` caches are not source assets and are not embedded.

On the model worker, missing or invalid external overrides fall back to the
embedded model. The five files are verified and extracted to
`%LOCALAPPDATA%/Medicon/cache/models/neon-<pack SHA-256>/` for the native texture
loader. Matching files are reused; corrupted extracted files are replaced.
FBX parsing, rig generation and native PNG registration then use the same
pipeline as an external model. This also supplies the first-person arm mesh.

`Material` option `Neon` is the display name of the existing alpha-test material;
it does not select a model asset. The default custom model is Neon regardless
of which of the two shader choices is selected.
