# GUI の拡張

`medicon::gui::Overlay` がゲーム上に描くもの（Edge-lit ArrayList HUD と Dropdown ClickGUI）をまとめます。DLL（`Client`）と `MediconPreview.exe` は同じ `Overlay` と Edge-lit 描画を使います。ClickGUI は、紫から赤へ移るアイコン付きヘッダー、四角い濃灰色パネル、有効な行全体のカテゴリ色、行内に展開する設定を使います。モジュールを `ModuleManager` に登録するだけで、カテゴリーごとのパネル・ON/OFF・キー割り当て・設定部品・保存が自動で用意されます。ClickGUI の寸法・色・時間は `include/medicon/gui/Theme.hpp`、実装仕様は [GUI_SPEC.md](GUI_SPEC.md)、ArrayList の全レイヤーは [EDGELIT.md](EDGELIT.md) を参照してください。

## 操作

| 対象 | 操作 | 結果 |
|---|---|---|
| ヘッダー | 左ドラッグ / 右クリック | パネル移動 / 折りたたみ |
| Module 行 | 左 / 右 / 中クリック | ON/OFF / 設定の展開 / キー割り当て待機 |
| 設定 | クリック・ドラッグ | Toggle、Slider、Dropdown、ColorPicker、Keybind |
| パネル内 | ホイール | スクロール |
| 上部中央の Social アイコン | 左クリック | Social 画面の開閉 |
| `Insert`（ClickGUI のキー） | | 開閉 |
| `End` | | オーバーレイを安全に停止 |
| `Esc` | | 待機の取消 → オーバーレイを閉じる → GUI を閉じる |

キー割り当て待機中: `Esc` 取消、`Backspace`/`Delete` 解除、Mouse4/5 も割り当て可能。
`End` は入力コールバック外のスレッドでフックとオーバーレイを停止します。直接 inject 時は、コールバックが終了し、フックとウィンドウプロシージャを安全に解除できた場合に DLL を unload します。unload 後は再度 inject してください。別プラグインのウィンドウプロシージャが Medicon を呼び得る場合やフック解除に失敗した場合はクラッシュ防止のため DLL を残し、理由をログに記録します。ローダー/API 経由ではホストが DLL の寿命を管理します。

旧テーマの標準配置が保存されている場合は、幅を広げたパネルが重ならないよう新しい標準配置へ移行します。手動で変更した配置は保持します。

## 自動生成される設定

| 型 | UI |
|---|---|
| `bool` | カテゴリ色の四角いチェックボックス |
| `int` / `float` | `minimum`〜`maximum` のスライダー（`step` に丸め、表示桁は `step` から決定） |
| `Choice` | ドロップダウン |
| `Color` | カラーピッカー（HSV + α） |
| `std::string` | 読み取り専用表示 |

`name` は表示名、`id` は保存キー、`visible = false` の項目は表示されません。設定値は即座に変わり、変更時に自動保存されます。名前が `Mode` の `Choice` 設定は ArrayList の Suffix になります（`Module::suffix()` を override して変更可）。ArrayList に出したくない Module は `set_show_in_arraylist(false)` を呼びます。

Watermark（`watermark`、Client カテゴリ、既定で ON）はゲーム中だけ左上に出る HUD です。メインメニューのボタンと同じガラスプレート（`draw_glass_plate`：ミルキーな白の塗り、白い 1px の縁、左の水色のアクセントライン、柔らかい青のドロップ）に、メインメニューと同じ DynaPuff で「Medicon」（21px、`#80C9FF`）とバージョン（11px、灰色がかった青）を載せます。バージョンは MediconLoader が起動時に取得・署名検証した更新サーバーの manifest の最新版（`MediconLoaderGetStatus` の `latest_version`）を使い、オフラインなどで取れなかったときは起動した Core のバージョン、ローダーなしで注入したときはビルドのバージョンを出します。Core 自身は通信しません。高さ 40・角丸 14・画面端から 16（すべて HUD スケール × `scale`）。影は `#A9D9FF` のぼかし、Glow は淡い青の光とアクセントラインのにじみで、どちらもメッシュをワーカーで1回作って使い回します。表示時は 250 ms の EaseOutCubic で不透明度 0→1・Y −5→0・拡大 0.97→1、OFF にすると同じ動きを逆に再生します。設定は `version`（ON）、`fps`（OFF、0.5 秒ごとに更新）、`ping`（OFF、取得元がまだないため現在は何も出ません）、`shadow`、`glow`、`scale`（0.75〜1.5）、`opacity`（0.5〜1.0）。項目が増えるとプレートの幅が合わせて伸びます。ゲーム画面の背景ブラーは描画基盤が対応していないため使わず、塗りと影で質感を出しています。DynaPuff は同梱の太さが Medium だけなので、太字にはしていません。

`mode` を `Japanese` にするとプレートの代わりに、注入アニメーションのタイトルと同じ見た目（`kawai.otf`、白い塗りに `#9C6BB8` の縁取り）で「めじこん」（30px）を出し、`glow` がオンなら周りに淡い紫の光を足します。光は文字をずらして重ねたリングで作るので、文字の形に沿います。詳細は文字の右に、読めるよう同じ縁取りを付けて並べます。動きは `Plate` と同じで、`shadow` は対象のプレートがないため設定から隠れます。

ArrayList 自体の設定は `accentA`（`#FF4FA0`）、`accentB`（`#8B6BFF`）、`glow`（1.0、0〜1.6）、`shadow`（1.0、0〜1.6）、`pulse`（ON）、`scale`（1.0、0.5〜2.0）の 6 つです。行は高さ 24・隙間 0、名前は Geist SemiBold 15px・モードは Geist Mono Medium 12px、右揃えの幅順。すべての寸法は実画面 px に `scale` だけを掛け、GUI Scale や DPI から独立しています。従来の位置・文字サイズ・背景表示・Suffix 表示などの個別設定は使いません。

## 追加のウィンドウ

```cpp
client.gui().add_panel("Debug", [&modules] {
    ImGui::Text("Modules: %zu", modules.all().size());
});
```

`add_panel()` の ImGui ウィンドウは GUI が開いている間だけ表示されます。

## モジュール固有の UI

`set_module_settings_renderer()` を登録した Module は、展開中に専用の ImGui ウィンドウを開きます。通常の設定も出す場合は `render_settings()` を呼びます。

```cpp
client.gui().set_module_settings_renderer("example", [](medicon::Module& module) {
    medicon::ClickGui::render_settings(module);
    if (ImGui::Button("Custom action")) { /* ... */ }
});
```

## 描画ホスト

```cpp
// 初期化: ImGui::CreateContext() → backend Init() → ClickGui::apply_theme()
// 毎フレーム:
// backend NewFrame()
if (overlay.prepare_frame()) { /* フォントアトラス再構築: フォントテクスチャを再作成 */ }
ImGui::NewFrame();
modules.tick(dt);
modules.render();
overlay.render();
ImGui::Render();
// 完成したワールドの上、ImGui パネルの下へ描く専用 D3D11 パス。
// edge_lit は gui::EdgeLitD3D11、引数は現在の描画ホストから渡す。
edge_lit.render(device, immediate_context, back_buffer, render_target, error);
// backend RenderDrawData(ImGui::GetDrawData()) → Present()
```

フォント再構築時はホストのテクスチャ再作成に加えて `edge_lit.invalidate_fonts()`、デバイス破棄時は `edge_lit.reset()` を呼びます。自動接続は D3D11 / D3D12（D3D11On12）、Preview は D3D11 でこのパスを実行します。外部 D3D11 ホストには `MediconRenderEdgeLitD3D11` を用意しています。

キーは Win32 virtual-key で `overlay.key_event(vk, down, repeat)` に渡します（GUI → Module ホットキーの順にルーティング）。戻り値が `true` のキーはゲームに渡さないでください。Mouse4/5 は `VK_XBUTTON1/2` として渡すと Module のキーに使えます。フォーカス喪失時は `modules.reset_keys()`。DLL では同じ順序を C API（`MediconPrepareFrame` → `MediconFrame` → `MediconRenderEdgeLitD3D11`、入力は `MediconKeyEvent`）で行います。

ClickGUI 表示中も `W/A/S/D`、Space、Shift、Ctrl で移動できます。文字入力欄・キー割り当て中、Social 画面、Medicon のメインメニューでは移動入力を遮断します。GUI 内のマウス操作は通常メッセージ・Raw Input・ボタン状態の直接取得を通じてゲームへ漏れないよう処理し、閉じる際は実カーソルをゲーム側の基準位置へ戻してから視点入力を再開します。GUI で押したボタンは、閉じた後も離すまで GUI 側が所有します。

ゲーム内部にもマウス入力のキューと視点更新があるため、対応する1.26.52.3では両方の入口でも遮断します。根拠とテストは [native-gui-input.md](native-gui-input.md) を参照してください。

## 上部中央のナビゲーション

`TopNav.hpp` の `TopNavItem` は、画面の上部中央に置くアイコンだけのボタンです。今は Social の 1 つだけで、バーのような背景は持ちません。

- 寸法は 44×38 rpx のクリック領域と 18 rpx のアイコンで、上端から 12 rpx の位置に置きます。既定のパネル（y = 88）とは重なりません。パネルを重ねた場合はボタンが手前になり、先にポインターを受け取ります。
- 待機中はアイコンのみです。ホバーで 1.5 rpx 浮き、ごく薄い角丸プレートと小さなラベル `Social` が出ます。押している間は 4% 縮みます。
- 選択中はアイコンの色が変わり、アイコンの下に 12×2 rpx のインジケーターが 200 ms で幅を広げながら現れます。Social 画面の淡い背景に乗るため、選択中の色は濃い青です。
- `ClickGui` は Social 画面を開いている間も描画を続けず、Social 画面の上に重ねる処理だけを `Overlay` が呼びます（`social_button_input` / `draw_social_button`）。ClickGUI から開いた Social ではボタンが画面の上に残り、もう一度押すと閉じます。ClickGUI を閉じたまま `Home` で開いたときと、サインインの必須画面では出しません。
- 通知は上部中央に出るため、通知の下にあるボタンとパネルはクリックを受け取りません。
- ボタンを増やすときは `TopNavIcon` に 1 つ足してアイコンを描き、`top_nav_rect(slot, count, ...)` で並べます。色・寸法・時間は `theme::top_nav` にまとめています。

## ClickGUI の描画負荷

この節の比較値は外観の刷新前に行った描画負荷改善の記録です。

パネルの影は、角とぼかしの影響が重ならない軸の中央部分を伸縮し、サイズ変更中も同じ形状データを再利用します。小さい矩形や大きいぼかしでは従来の寸法別計算を使います。描画に使わない内部頂点を除き、レイアウト用バッファも再利用します。スクロール領域外の設定行は描画を省略しますが、入力判定とオーバーレイ配置用の行情報は保持します。

DLL のネイティブマウス状態の定期確認は 100ms 間隔です。GUI 開閉とフォーカス変更時の同期、ゲーム側の再 grab 抑止は引き続き即時に処理します。GUI 表示中は相対 Raw Input をレガシーの移動・ホイールへ展開しません。巨大な相対デルタがスクロールになり、毎フレーム影と頂点を作り直して FPS が落ちるのを防ぎます。本物のホイールはそのまま使います。

2026-10-26 の Release 検証では CTest 全 15 件が成功し、主要 8 シナリオの画像は変更前と全ピクセル一致しました。同じ CPU 計測を変更前後で交互に 3 回行った中央値は、デモ GUI の影あり表示が 0.3285 → 0.1206ms、8 枚の影の高さを変える負荷試験が 8.7175 → 0.1071ms でした。後者の形状再計算は 160 フレームで 1,280 → 1 回です。これらは CPU の描画データ生成時間であり、Minecraft 内の FPS や GPU・Present の時間は含みません。`gui` テストはサイズ変更時の再利用回数と Gaussian モデルに対する描画精度、`mouse_capture` テストは高 FPS 時の確認頻度と即時の開閉・復元再試行を検証します。

## スタンドアロンのプレビュー

`MediconPreview.exe` は Minecraft なしで GUI を確認する Direct3D 11 アプリです。通常起動では `%LOCALAPPDATA%\Medicon\config.json` を読み書きします（読み込みに失敗したファイルは上書きしません）。

| 引数 | 内容 |
|---|---|
| `--demo` | 表示確認用のダミー Module を登録 |
| `--scenario <name>` | 指定シナリオを非表示ウィンドウ・オフスクリーンで実行（下表） |
| `--screenshot <out.png>` | シナリオの結果を保存（複数時刻のシナリオは `_t060` などを付加） |
| `--size WxH` / `--dpi N` | クライアントサイズ / DPI の上書き |
| `--arraylist-scale N` | HUD の物理 px スケール（0.5〜2）。添付参考との2倍表示比較は `2` |
| `--bg white\|gray\|black\|day\|night\|<画像>` | 単色、昼／夜の Minecraft 風背景、または画像ファイル |
| `--fixed-dt <s>` / `--fps <n>` | 固定時間刻み / フレームレート上限 |
| `--gpu-timing [--no-shadow]` | UI 全体と影の GPU 時間（60 フレーム捨て 600 計測、中央値・p95） |
| `--simulate-device-lost <frame>` | デバイスを破棄・再作成 |
| `--debug-layer` | D3D11 debug layer（Windows の「グラフィックス ツール」が必要） |
| `--input-test` | Win32 メッセージによる入力の自動テスト |
| `--smoke-test` | 5 フレーム描画して終了（設定ファイルに触れない） |
| `--log <file>` | ログをファイルにも出力 |

| Edge-lit シナリオ | 確認内容 |
|---|---|
| `edgelit_reference` | 参考の 9 行、Elytra Fly がパルス到着直後に点灯 |
| `edgelit_idle` | 全行が静止・無点灯 |
| `edgelit_on` / `edgelit_off` | 60 / 120 / 220 / 320 / 500 / 1600ms の連続画像 |
| `edgelit_mode` | Velocity のモード変更に伴う幅・並び順の追従 |
| `edgelit_no_effects` | `glow = shadow = 0`、基本のプレート・文字・チューブを保持 |
| `edgelit_no_pulse` | パルス OFF、スライドは実行 |
| `edgelit_scale` | `scale = 1.25`、小数の文字サイズも再ラスタライズ |
| `edgelit_single` / `edgelit_empty` | 1 行の A 色、空リストで残像なし |

ClickGUI のシナリオは `clickgui_reference`（代表的な機能を有効化し Example の設定を展開）、`clickgui_social`（上部中央の Social ボタンの待機・ホバー・押下・選択）、`closed`、`open`、`collapsed`、`settings`、`dropdown`、`colorpicker`、`scroll`、`arraylist_on`、`arraylist_off`、`shadow_on`、`shadow_off`、`shadow_profile` です。

リポジトリのルートから、昼・夜・白背景を同じサイズで撮影できます。出力は 1 倍の実画面 px なので、2 倍で書き出された参考画像と比較するときは参考を半分の寸法へ合わせます。

```powershell
$preview = './out/build/local/bin/MediconPreview.exe'
& $preview --scenario edgelit_reference --size 960x640 --bg day --fixed-dt 0.0166667 --screenshot out/edgelit_day.png
& $preview --scenario edgelit_reference --size 960x640 --bg night --fixed-dt 0.0166667 --screenshot out/edgelit_night.png
& $preview --scenario edgelit_idle --size 1920x1080 --bg white --fixed-dt 0.0166667 --screenshot out/edgelit_white.png
& $preview --scenario edgelit_on --size 960x640 --bg night --fixed-dt 0.0166667 --screenshot out/edgelit_on.png
& $preview --scenario edgelit_off --size 960x640 --bg day --fixed-dt 0.0166667 --screenshot out/edgelit_off.png
& $preview --scenario edgelit_scale --size 1920x1080 --dpi 144 --bg day --fixed-dt 0.0166667 --screenshot out/edgelit_scale.png
```

補助スクリプト: `tools/capture_screenshots.ps1`（既定は従来の ClickGUI シナリオ、`-Scenarios` で選択可能）、`tools/verify_screenshot.ps1`（ClickGUI の色・寸法・文字の縦位置）、`tools/resize_stress.ps1`（連続リサイズ）、`medicon_shadow_reference`（ClickGUI の影の参照画像と比較）。Edge-lit の昼夜と各効果は上の専用シナリオで検証します。
