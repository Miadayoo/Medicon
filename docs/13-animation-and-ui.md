# Animation and immediate UI

```cpp
Tween<float> opacity_{0};
void on_enable() override {
    opacity_.to(1.F, std::chrono::milliseconds(260), Easing::OutCubic);
}
void on_ui(UIContext& ui) override {
    auto panel = ui.glass_panel("quick", {34,380,300,160}, Glass::regular().radius(20));
    panel.label("Quick controls");
    if (panel.button("notify", "Notify")) notification::show("Ready", "Clicked once");
    panel.progress("progress", opacity_.value());
}
```

Tween は float/double/Vec2/Color の所有値だけを持ち、host の monotonic clock をサンプルします。
途中で to を呼ぶと現在値から続きます。Linear/OutCubic/OutExpo/InOutCubic。
Windows の「アニメーション効果」OFF では即時に目標へ移り、本体の共通 tween/follow/spring も同じ設定を参照します。
全 UI の個別の演出時間を強制停止する仕組みではありません。既存の独自タイムラインにはこの範囲外のものがあります。
disable 後に host から Tween の callback は呼ばれません。再有効化時の初期目標は on_enable で指定してください。

on_ui は ClickGUI が開き、ゲームウィンドウに focus があり、Main Menu/Social/Disconnect/Splash が前面にない間だけ呼びます。
通常 HUD は on_render。UI は論理 DIP、HUD の旧 API は物理 px なので `.logical()` を明示します。
Panel は move 専用 RAII。関数を抜けると End、例外や未終了 panel も host が callback 末尾で閉じます。
Panel、UIContext、RenderContext をメンバーや thread に保存して使い回さないでください。
widget は token と callback phase を検証し、古い token は無効です。

label/button/progress は既存 ImGui の layout、clip、activation、keyboard navigation を使います。
panel に keyboard focus または hover がある間は ClickGUI のゲーム移動許可を止めます。
`input::key_down/cursor_position` は on_ui の focus を持つ panel 内だけで使えます。
アプリが focus を失ったときと ClickGUI を閉じたときは held key をクリアします。

panel ID / button ID はモジュール内で安定させ、同じ frame の重複 panel ID は避けてください。
host の UI window は 64 slot の固定 pool。ID+generation ごとの割当は disable/reload で解放し、毎 frame 新しい ID を作ると上限で拒否します。
raw ImGui pointer や GPU handle は不要です。
