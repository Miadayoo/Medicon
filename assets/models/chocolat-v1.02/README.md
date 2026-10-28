# Chocolat v1.02

Source: the user's `C:/Users/hina/Downloads/Chocolat_v1.02/Chocolat_v1.02`.
The standard `FBX/Chocolat.fbx` and five original files from `Texture/` are copied
unchanged. The alternate `Chocolat_kaihen.fbx`, PSDs, UV maps, Unity package and
preview images are not runtime assets.

`model.json` maps the five FBX material names to the supplied PNGs because the
FBX retains old exporter references (`face.psd`, `Chiffon_Body.png`, etc.). It also
maps the humanoid limb roles to `UpperArm.L/R`, `LowerArm.L/R`, `UpperLeg.L/R` and
`LowerLeg.L/R`. Scale 1.5 gives a character height of approximately 1.8 blocks;
yawOffset 180 matches the existing player orientation convention.

`cmake/EmbedModel.cmake` pins all seven resource files. They ship in the same
Core DLL as Neon, so the loader does not need another asset download. The worker
extracts the selected model to a content-addressed cache, then builds its static
mesh, skeleton, generated motion clips and first-person arms.

The source contains 20 meshes with 83,290 triangles and 290 scene nodes. Duplicate
skin bindings across clothing meshes are merged exactly (5,380 entries become
288 unique bindings). ufbx compensates FBX scale inheritance, and topology-derived
bind normals repair zero authored normals in the hair mesh.

Import verification: 73 native mesh chunks, 96,654 skin vertices, 18,613 arm
vertices, 14,334 arm triangles; maximum rest-pose position error below 0.000001
block. Game appearance, Unity/lilToon effects, physics, expressions and GPU cost
are not established by these CPU/import tests. The current renderer uses the
five diffuse PNGs with its existing native entity materials.
