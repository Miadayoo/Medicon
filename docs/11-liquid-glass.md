# Liquid Glass

```cpp
auto draw = render.logical();
draw.glass({24,24,300,86}, Glass::regular().radius(24).refraction(.7F));
draw.text({44,54}, "Medicon", Color::white(), 20);
```

これは `src/gui/LiquidGlass.cpp` の既存光学処理です。BlockCount と同じ
`publish_liquid_glass → EdgeLitD3D11 → LiquidGlassD3D11` を通り、D3D12 でも既存 D3D11On12 を使用します。
スクリプト用のシェーダー・レンダラー・背景取得・テーマ・品質キャッシュは追加していません。

| 指定 | 実効対応 |
|---|---|
| regular | 既存 native optics |
| radius | 既存 rounded-box uniform。半短辺まで clamp、旧 native widget の既定 capsule は維持 |
| refraction | 0..1 の既存 lens。屈折と吸収の両方が変化 |
| opacity | 0..1 の既存合成 alpha |
| quality(Inherit) | 共通経路を使う。独立した quality tier は存在しない |
| clear/frosted/prominent | 既存 preset がないため診断と GlassPlate fallback |
| blur/tint/saturation/highlight/border/shadow/その他 quality | 独立入力がないため診断と fallback |

`feature::available(Capability::Glass)` は renderer の初期化成功と共通アクセシビリティ条件に依存します。
GlassBlur/GlassTint/GlassQuality は false です。機能がないときも読みやすい既存 GlassPlate を使います。
未対応指定の値を native 効果へ適用したとは扱いません。

描画は ImGui より前の scene を一度だけ取得し、既存 downsample/blur を一回分実行して全パネルを合成します。
重なった Glass 自身を背景として再取得しません。ImGui の別ウィンドウは屈折対象になりません。
背後ではなく前面に不透明な native UI がある場合、HUD は隠れます。
UI パネルは ClickGUI に重ならない位置へ配置してください。

Windows の transparency OFF / High Contrast は native optics を止め、共通 plate を不透明にします。
同じ方針を本体の Glass/BlockCount と scripts に適用します。架空の blur や quality 設定は追加しません。

[ソースに基づくマッピング](19-existing-liquid-glass-bridge.md) / [実装前の一覧](internal/glass-renderer-inventory.md)
