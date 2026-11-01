# Public API (400 / ABI 1)

公開の入口は `<medicon.hpp>` です。以下は実際の公開ヘッダーに対応します。
すべてのホストアクセスはホストが呼ぶ callback 内だけで行います。自作 thread や DLL 初期化処理から呼びません。

| API | 意味・制約 |
|---|---|
| `Module::on_enable/on_disable/on_tick/on_render/on_ui` | 有効期間と描画 callback。on_tick は引数なし |
| `setting<T>(id,label,initial).range(min,max,step)` | bool/int/float/Color/string。コンストラクタで宣言。`.get()` は host の値を読む |
| `chat::print`, `log::info/warn/error` | ローカルチャットと診断。chat は実際に利用可能なときだけ成功 |
| `game::local_player()` | optional な読み取り専用コピー。ゲームポインタは返さない |
| `RenderContext::text/fill_circle/center` | v0.1 と同じ物理 px |
| `RenderContext::logical()` | 現在の UI scale で論理 DIP を物理 px に変換する facade |
| `viewport_size()` | context の単位での画面サイズ |
| `fill_rounded_rect/stroke_rounded_rect/line` | 既存 ImGui 描画リストに転送 |
| `glass(Rect,Glass)` | 同じ native Liquid Glass にコピーしたコマンドを送信。native 成功で true、fallback/拒否で false |
| `UIContext::glass_panel` | RAII の Panel。論理 DIP。callback 内限定 |
| `Panel::label/button/progress` | 既存 ImGui widget。button はクリック・キーによる一度の activation で true |
| `Tween<float/double/Vec2/Color>` | host の単調時計で補間。`.to(value, chrono duration, Easing)`、`.value()` |
| `after/every/cancel_timer` | Module の callback。chrono duration、ID は uint64。無効化で自動解放 |
| `listen<T>` | TickEvent/RenderEvent/KeyEvent/KeyPressed/GameJoined/GameLeft/ScreenResized |
| `storage::get/set` | ID ごとに独立した UTF-8 文字列。get は optional。set はキャッシュ更新と保存予約 |
| `notification::show` | 既存の通知 queue。受理されたとき true |
| `theme::color` | TextPrimary/TextSecondary/Accent/Surface/Outline の既存テーマ |
| `theme::reduced_motion/reduced_transparency/high_contrast` | 共通の Windows アクセシビリティ方針 |
| `input::cursor_position/key_down` | on_ui のフォーカスされた panel だけ。無効時は {-1,-1}/false |
| `sound::play_ui` | Success の既存チャイムだけ対応。設定・rate limit に従う |
| `feature::available(Capability)` | 現在の host/device の実効機能。権限境界ではない |
| `features::is_available` | 旧 chat/render/local_player の API も維持 |
| `sdk::api_version/abi_version` | 定数としても関数形式 `sdk::api_version()` としても利用可能 |

`Rect{x,y,width,height}`。Color は 0..1 RGBA。`Color::rgb/rgba` は 0..255 の byte を変換します。
座標・スタイルに NaN/inf、非正の幅高さ、範囲外の色を渡すと拒否します。
描画は callback ごとに 2048 コマンド、Glass/UI はモジュールごとに 20 枚、全体は 64 枚（native HUD 用 1 枠を確保）です。

RenderContext は on_render/RenderEvent、UIContext と Panel は on_ui の間だけ有効です。
GPU、フォント、背景キャプチャ、widget state をスクリプトへ公開しません。
source_location により Glass の非対応指定は呼出元のファイルと行を表示します。
