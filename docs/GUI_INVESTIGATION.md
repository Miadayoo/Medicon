# GUI 実装前調査（Phase 1）

調査日: 2026-10-25。行番号は変更前のソース（ベースライン）のもの。

## 1. ビルド

| 項目 | 内容 |
|---|---|
| ビルドシステム | CMake 3.25+ / C++20（`CMakeLists.txt:1-6`）。依存は FetchContent でコミット固定（json、imgui、minhook: `CMakeLists.txt:18-24, 71-77`） |
| ツールチェーン（この環境） | `.tools` 内の LLVM MinGW（clang 20, UCRT）+ Ninja。`tools/build.ps1:21-47` が自動選択。CMake は `.tools/python/cmake/data/bin/cmake.exe`（`.tools/python/bin/cmake.exe` は Python ラッパーで単体では動かない） |
| ターゲット | `Medicon`（DLL, `CMakeLists.txt:95`）、`MediconPreview`（WIN32 exe, `:101`）、静的ライブラリ `medicon_core` `:49` / `medicon_memory` `:39` / `medicon_imgui` `:79` / `medicon_gui` `:87` / `medicon_runtime` `:91` |
| テスト | 自前の軽量 `check()/require()` 形式 + CTest（`:58-65, 112-122`）。core / signatures / preview_smoke / runtime / dll_lifecycle の5件 |
| Release ビルド | `powershell -ExecutionPolicy Bypass -File D:\code\Medicon\tools\build.ps1 -Configuration Release`（ビルド先 `out/build/local`、CTest も実行） |
| Debug ビルド | `powershell -ExecutionPolicy Bypass -File D:\code\Medicon\tools\build.ps1 -Configuration Debug` |
| VS2022 環境 | `cmake --preset windows && cmake --build --preset release && ctest --preset release`（`CMakePresets.json`） |

ベースライン（クリーンビルド, `out/build/baseline-{Debug,Release}`）:

| 構成 | 結果 | 警告 | テスト |
|---|---|---|---|
| Debug | 成功 | 0 | 5/5 成功 |
| Release | 成功 | 0 | 5/5 成功 |

`medicon_imgui` には `medicon_target()` の警告オプションが掛からない（サードパーティ扱い）。

## 2. Renderer

- **DLL は描画 API を一切フックしていない。** `Present` / `ResizeBuffers` / `ExecuteCommandLists` のいずれもフックなし。README「ゲームとの接続」節とヘッダー `include/medicon/platform/ClientApi.h:28-30` の通り、**ホストが ImGui コンテキスト・レンダラー・NewFrame/Render・入力 backend を所有**し、DLL は `MediconFrame(context, dt)`（`src/platform/ClientApi.cpp:63-80`）の中でホストの ImGui フレームに描画コマンドを積むだけ。
- したがって DLL 側で D3D デバイス・テクスチャ・シェーダーを作ることはできない（デバイスを知らない）。D3D11/D3D12 の差は ImGui のホスト backend が吸収する。
- Preview: D3D11 + ImGui `imgui_impl_win32` / `imgui_impl_dx11`（`src/preview/Main.cpp:137-242`）。`D3D11CreateDeviceAndSwapChain`（HARDWARE→WARP フォールバック, `:268-292`）、`DXGI_SWAP_EFFECT_DISCARD` 2バッファ。
- Resize: `WM_SIZE` で保留 → 次フレーム頭で RTV 解放 → `ResizeBuffers` → RTV 再作成（`:306-319, 332-338`）。
- パイプラインステートの保存/復元: `ImGui_ImplDX11_RenderDrawData` が `BACKUP_DX11_STATE` で全ステートを退避・復元（imgui `backends/imgui_impl_dx11.cpp:202-226`）。
- D3D12 backend は Preview に存在しない。

## 3. ImGui

- バージョン **1.91.9b**（`IMGUI_VERSION_NUM 19191`）。DLL とホストの一致を `MediconImGuiVersion()` で確認する設計（`ClientApi.cpp:37`）。
- フォントローダーは stb_truetype（FreeType 無効, `imconfig.h:87`）。**1.92 未満なので動的フォント（`RendererHasTextures`）は使えない**。アトラスは NewFrame 前にしか変更できず、変更後は backend のフォントテクスチャ再作成が必要（DX11: `ImGui_ImplDX11_InvalidateDeviceObjects()` / `CreateDeviceObjects()`）。
- DX11 backend は `RendererHasVtxOffset` 対応（16bit index で 64k 超の頂点可）、`ImDrawCmd::UserCallback` 対応（`imgui_impl_dx11.cpp:269-276`）。
- `ImDrawList` に角ごとの角丸フラグ（`ImDrawFlags_RoundCornersTopLeft` 等, `imgui.h:3059-3068`）と Clip スタック（`PushClipRect`, `imgui.h:3120`）、`AddCallback`（`imgui.h:3194`）がある。

## 4. バックバッファ

- Preview: `DXGI_FORMAT_R8G8B8A8_UNORM`（`Main.cpp:271`）、RTV は `nullptr` 記述子 = 同フォーマット（UNORM）。sRGB 変換なし → ImGui の頂点色はガンマ空間でそのまま出力・合成される。
- DLL: ホスト依存（不明）。

## 5. Module

- 登録: `ModuleManager::add<T>()`（`include/medicon/core/ModuleManager.hpp:23-32`）、重複 ID は例外。組み込み登録は `register_builtin_modules()`（`src/modules/BuiltinModules.cpp:7-9`）。
- 登録順: `ModuleManager::all()` の vector 順（`:34-35`）。
- ID と表示名: `Module::id()`（保存キー）と `Module::name()`（表示）が別（`Module.hpp:20-21`）。
- カテゴリー: `enum class Category { Visual, Movement, Player, Misc }`（`Setting.hpp:9`）、名前は `category_name()`（`Module.cpp:10-18`）。列挙順 = enum 順。
- メタデータ: 説明文のみ。ArrayList 非表示フラグ・Suffix は**無い**。
- ON/OFF: `set_enabled()` → `on_enable()/on_disable()`（`Module.cpp:33-38`）。**トグルイベントは EventBus に流れない**。

## 6. Setting

`struct Setting`（`Setting.hpp:27-41`）。値は `std::variant<bool, int, float, Choice, Color, std::string>`。

| 型 | 属性 |
|---|---|
| bool | なし |
| int / float | `minimum` / `maximum` / `step`（`add_setting` で検証, `Module.cpp:47-57`） |
| Choice | `index` + `options`（Enum 相当） |
| Color | RGBA float（α は常に持つ。α有無の属性なし） |
| std::string | なし |
| 共通 | `id`（保存キー）、`name`（表示）、`description`、`visible`（表示条件。静的フラグ） |

精度（表示桁数）の属性は無い。Keybind は Setting 型ではなく `Module::keybind()`（VK, 0=未割当）。

## 7. Config

- `ConfigStore::load/save`（`ConfigStore.hpp:15-24`, `ConfigStore.cpp:123-216`）。JSON（nlohmann）、スキーマ v1:
  `{"version":1,"modules":{"<moduleId>":{"enabled":bool,"keybind":int,"settings":{"<settingId>":value}}}}`
- load は全体を検証してから一括適用（不正値なら何も適用しない）。未知の ID は無視（前方互換）。save は一時ファイル → `MoveFileExW` で原子的置換。
- 保存タイミング: Client 停止時（`Client.cpp:37-45`）、GUI の Save ボタン、Preview の終了時（`Main.cpp:103-114`）。
- キー命名: snake_case の ID。保存先 `%LOCALAPPDATA%\Medicon\config.json`。
- 控え: `tests/fixtures/baseline_config.json`（ベースライン Preview が実際に書き出したファイルと同じスキーマ。互換テスト用に値を非既定値へ変更）。

## 8. EventBus

`include/medicon/core/EventBus.hpp:16-82`。型別 publish/subscribe、RAII 解除、単一スレッド。**どのコンポーネントも publish していない**（Module ON/OFF・Setting 変更・キー・描画・Resize いずれのイベントも未定義）。

## 9. Keybind

- 表現: Win32 VK（0..255, 0=None）（`ModuleManager.hpp:39-40`）。新規押下のみでトグル（リピート無視, `ModuleManager.cpp:17-26`）。
- キー名変換: `key_name()`（`src/gui/ClickGui.cpp:91-108`, ImGuiKey→VK 対応表 `:44-89` 経由で `ImGui::GetKeyName`）。
- マウスボタン: 未対応（ホストはキーイベントのみ渡す）。
- GUI 開閉キー: `VK_INSERT` 固定（`Client.cpp:61`, `Main.cpp:345`）。

## 10. 入力

- DLL: マウスはホスト ImGui の IO（ホストの Win32 backend）。キーは `MediconKeyEvent(vk, pressed, repeat)` → `Client::key_event`（`Client.cpp:59-68`）。戻り値「GUI 表示中は 1」でホストがゲーム入力を遮断する契約（`ClientApi.h:31-33`）。カーソル表示はホスト責任。
- Preview: `WndProc` で `ImGui_ImplWin32_WndProcHandler` → 独自に VK を ModuleManager へ（`Main.cpp:321-373`）。Preview は `Client` を使わず ModuleManager+ClickGui を直接持つ（Insert 処理が二重実装）。
- `imgui_impl_win32` は DBLCLK を押下として扱い、ドラッグ中 `SetCapture`、`WM_KILLFOCUS` で `AddFocusEvent(false)`（ボタン状態を解放）、XButton→Mouse4/5、ホイールは `delta/120` の float。

## 11. 独自 GUI パネル

`ClickGui::add_panel(title, callback)`（`ClickGui.hpp:28-29`, `ClickGui.cpp:231-238`）: GUI 表示中だけ ImGui ウィンドウとして描画。`set_module_settings_renderer()`（Module 固有 UI 差し替え）と `set_config_actions()`（Load/Save ボタン）もある。描画・ヒットテスト・ドラッグは ImGui ウィンドウ任せで、再利用できる自前実装は無い。

## 12. フォント

FontManager は無い。ImGui 既定フォント（ProggyClean 13px）のみ。同梱フォントなし。日本語グリフなし。
システム: `segoeui.ttf` / `seguisb.ttf` / `SegUIVar.ttf`（Windows 11）/ `YuGothR.ttc`, `YuGothM.ttc`（Yu Gothic UI を含む）。**Meiryo UI は未インストール**。

## 13. 描画ユーティリティ

ImGui `ImDrawList` のみ（角ごと角丸・Clip スタック・テクスチャ・コールバックあり）。カスタムシェーダーは DLL からは差し込めない（デバイス不明）。Preview は自前の D3D11 なので `AddCallback` で差し込み可能。

## 14. Preview

エントリー `wWinMain`（`Main.cpp:400-413`）、引数は `--smoke-test` のみ。実際の `register_builtin_modules()` を使う（ダミーなし, `:97-101`）。ウィンドウ 1240×820, `ImGui_ImplWin32_EnableDpiAwareness()`（Per-Monitor DPI）。

## 15. テスト

フレームワークなし。各テストは `main()` + 例外で失敗報告（`tests/core_tests.cpp:24-26, 291-303`）。

## 16. 既存 ClickGUI / HUD

- 既存 ClickGUI: `src/gui/ClickGui.cpp`（455行）/ `include/medicon/gui/ClickGui.hpp`。単一ウィンドウ＋3カラム（カテゴリー/一覧/詳細）＋検索＋Load/Save。**仕様で禁止のサイドバー／単一巨大ウィンドウ型なので置き換える。**
- 影響箇所: `Client`（`gui()`/`open()`/`capturing_keybind()`）、Preview、`ClientApi.cpp:90`、`runtime_tests.cpp:64-70`（起動時 GUI が開いている前提）、`dll_tests.cpp:185-190`、`docs/gui.md`。
- HUD: なし。

## 仕様項目 → 再利用する既存クラス/関数

| 仕様 | 再利用するもの | 追加するもの |
|---|---|---|
| Module 一覧・登録順・カテゴリー | `ModuleManager::all()`, `Module::category()`, `category_name()` | `Category::Client`（ClickGUI/ArrayList 用） |
| 表示名 / 内部ID | `Module::name()` / `Module::id()` | なし |
| ArrayList 非表示 | — | `Module::show_in_arraylist()`（既定 true） |
| Suffix | `Setting`（Choice, name=="Mode"） | `Module::suffix()`（virtual） |
| Setting 型と範囲 | `Setting` の variant・`minimum/maximum/step/visible` | なし（精度は step から導出） |
| Keybind | `Module::keybind()/set_keybind()`、`ModuleManager::key_event()` | キー名表（VK→名前）を GUI 共通基盤へ |
| Config 保存 | `ConfigStore`（スキーマ変更なし） | ClickGUI/ArrayList を Module 化し、その Setting として保存（パネル位置は非表示 Setting） |
| ON/OFF 検出 | `Module::enabled()` | 毎フレーム差分（トグルイベントが無いため） |
| 描画 | ImGui `ImDrawList` | 共通描画ユーティリティ |
| 入力 | ImGui IO（マウス）、`Client::key_event`（キー） | GUI 側キールーティング（Esc 優先順位・待機） |
| 独自パネル | `ClickGui::add_panel`、`set_module_settings_renderer` | 維持（ImGui ウィンドウのまま） |
