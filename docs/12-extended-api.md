# Services and events

```cpp
using namespace std::chrono_literals;
void on_enable() override {
    after(500ms, [] { notification::show("Ready", "Script enabled"); });
    every(2s, [] { log::info("periodic"); });
    listen<GameJoined>([](const GameJoined& e) { (void)e.world_identity; });
    listen<ScreenResized>([](const ScreenResized& e) { (void)e.width; });
    auto old = storage::get("label").value_or("Medicon");
    storage::set("label", old);
}
```

タイマーは host の tick で発火します。一度の停止で大量に追いかけて実行しません。
遅延は 0..7日、repeat は 1ms..7日、モジュールあたり 64 個まで。
every(0) は拒否。cancel_timer、無効化、失敗、reload/unload で callback と DLL 内 closure を解放します。
callback 内の例外は DLL 内で捕まえ、そのスクリプトを無効にします。

GameJoined/GameLeft はコピー済み player/world identity の変化から生成します。
ScreenResized は初回 tick と物理サイズまたは scale の変更時、論理 DIP と scale を渡します。
KeyPressed は game へ渡せる未消費キーの down イベントです。repeat フラグを確認できます。
GUI が消費するキーをグローバルな raw input として渡しません。旧 KeyEvent も維持します。

Storage は module ID ごとの JSON、256 KiB（シリアライズ全体）、1024 keys、key 128 bytes までです。
key はパスではなく、`..`、slash、backslash、NUL は拒否します。値は UTF-8。
読み込みはビルド worker、読み取りはメモリ、書き込みは専用 worker の一時ファイル＋atomic replace。
前回分を `.bak` に保存し、破損ファイルは `.corrupt` に退避して診断します。
set の true は保存予約の成功です。ディスク障害は監査ログへ記録します。秘密の保管庫ではありません。
on_disable からの最後の set にも対応します。

通知は既存 ToastManager（最大表示3、queue32）。共通 500ms rate limit、in-game 通知設定、サウンド設定に従います。
Level はメタデータです。既存 System toast の見た目を使い、severity 専用の新しい表示経路はありません。
通知に callback を保持せず、ID+generation で disable/reload 時に削除します。
`sound::Builtin::Success` だけ既存の通知音へ接続し、Click/Error は非対応診断を出します。

テーマの色は既存 Theme.hpp の同じ token です。個別の script theme はありません。
機能の有無は [API リファレンス](03-api-reference.md) と Manager の Capabilities で確認できます。
