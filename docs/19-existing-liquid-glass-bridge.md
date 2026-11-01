# Existing Liquid Glass bridge: source-backed mapping

事前調査: [glass-renderer-inventory.md](internal/glass-renderer-inventory.md)。以下は実装後の対応表です。
Script専用renderer/shader/theme/quality profileは追加していません。実ゲームでの確認状況は QA 記録を参照してください。

| Public API | 実在する接続先 | 変換・対応方針 |
|---|---|---|
| `RenderContext::glass` | `gui::publish_liquid_glass` → `EdgeLitD3D11::render` → `LiquidGlassD3D11::render` | Frameコピー。既存publisherとGPU renderer内をbatch化し、同じsceneを使う |
| `UIContext::glass_panel` | 上と同じnative背景 + 既存ImGui window/widget/input | UIの表示・focus・clipはhost所有。Panelは呼出中だけ有効 |
| `Glass::regular` | BlockCountと同じ`LiquidGlassFrame`の既定optics | 独立したScriptテーマを作らない |
| clear / frosted / prominent | 独立した既存presetなし | 提案文法を保持してもunsupported診断を返し、既存GlassPlateへfallback。光学presetの実装済みとしない |
| `.radius` | `LiquidGlass.cpp`の既存uniform `p.form[0]` | Frameに任意角丸の値だけを追加する。既定値は従来のcapsuleを維持。新shaderなし |
| `.refraction` | `LiquidGlassFrame::lens` | [0,1]、屈折+吸収の連動値であることを公開Docsに明記 |
| `.opacity` | `LiquidGlassFrame::opacity` | [0,1]。そのまま既存premultiplied合成 |
| `.blur`, `.saturation` | 可変の既存入力なし | unsupported + 既存plate fallback |
| `.tint`, `.border`, `.shadow`, `.highlight` | GPU側に独立入力なし | unsupported + 既存plate fallback。黙って成功しない |
| `.quality` | 既存tierなし | Inheritのみ。別tierはunsupported。架空の品質設定を追加しない |
| fallback | `gui::draw_glass_plate(ImDrawList*, ImVec2, ImVec2, float, const GlassPlate&)` | Main Menu / Socialと同じ実装とtheme::glassを使用 |

既存GPU処理はImGuiより前のsceneを取得する。重なるImGuiウィンドウまでを屈折する新しい背景取得経路は追加しない。
GUI内でnative光学結果が別の不透明なウィンドウに隠れる場合は既存plateの可読なsurfaceを使う。

旧API 100 / ABI 1は維持する。新Host/Plugin関数は末尾へ追加し、旧DLLには旧prefix sizeを提示する。
世代・phase・finite・rect・quota・capability検査はhostで行い、GPU生ポインタは公開しない。

検証状況は [acceptance record](16-acceptance-tests-v02.md) に記録しています。
