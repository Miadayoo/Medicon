# Script SDK v0.4: source inventory

調査日: 2026-11-01。添付仕様の仮名・モックではなく、この作業ツリーの実ソースを調査した記録。
変更前の基準: Script SDK 0.1、API 100、ABI 1。Windows x64 LLVMビルドの66 CTestが前回作業で成功。
MCBE実機への注入・MSVC実行・GPU性能の全組合せは未検証。

## Scripts / ABI / lifecycle

| 実ファイル | 実シンボル・署名 | 責任 |
|---|---|---|
| `include/medicon/scripts/ScriptServices.hpp`, `src/scripts/ScriptServices.cpp` | `ScriptPaths::initialize() const`, `ScriptDiscovery::scan(const ScriptPaths&, std::vector<ScriptDiagnostic>&)` | config.jsonの親/Scripts、SDK、Cache/build、Logs。直下cppとディレクトリ単位の発見 |
| `src/scripts/ScriptCompiler.cpp` | `ScriptCompiler::build(ScriptSource, std::atomic_bool&, bool) const` | MSVC /MTまたはLLVM-MinGW -static。別プロセス、依存ハッシュ、compile_commands.json |
| `src/scripts/ScriptProcess.hpp` | `detail::run(...) -> ProcessResult` | Windows Job Object、時間・メモリ・プロセス数制限、キャンセル |
| `src/scripts/ScriptPluginInspector.cpp` | `inspect_plugin(const fs::path&) -> MediconScriptManifest` | PEをファイルとして解析。LoadLibraryより前にx64/API/ABI/import/exportを検査 |
| `src/scripts/ScriptRuntime.cpp` | `ScriptPluginLoader::load(const BuildResult&, const ScriptTrustManager&, uint64_t)` | 承認したSHA-256のDLLだけをロックしてLoadLibraryExW |
| `src/scripts/ScriptServices.cpp` | `ScriptTrustManager::approved/approve/revoke` | source+binaryハッシュの事前承認、script-trust.json |
| `src/scripts/ScriptRuntime.cpp` | `ScriptModuleAdapter`, `drain() noexcept` | 本体Moduleへの変換、Scriptsカテゴリ、コールバックの再帰mutex、DLL内で生成・破棄 |
| `src/scripts/ScriptManager.cpp` | `ScriptManager::worker()`, `pump()`, `ScriptReloadCoordinator::replace(...)` | 500ms debounce、フレーム境界交換、旧DLL保持とロールバック |
| `sdk/include/medicon/abi.h` | `MediconHostApi`, `MediconPluginApi`, `MediconScriptManifest` | ABI 1。x64で旧Host 96 bytes、Plugin 64 bytes、Manifest 192 bytes。旧ヘッダーはsizeの完全一致を要求 |
| `sdk/include/medicon.hpp` | `Module`, `Setting<T>`, `RenderContext`, `MEDICON_MODULE` | STLはDLL内だけで使用。旧描画座標は物理ピクセル。Tick/Render/Keyイベントあり |
| `src/client/Client.cpp` | `Client::tick(float)`, `render()`, `game_tick(...)` | Present側のtick/renderとゲームスレッドのコピー済みsnapshotを接続 |
| `src/core/ConfigStore.cpp` | `ConfigStore::load_module(Module&, std::string&, bool*) const` | module_id+setting_idで保存・復元。未ロードモジュールも保存値を維持 |
| `src/gui/ScriptScreen.cpp` | `attach_script_gui(...)`, `render_script_screen(ScriptManager&)` | Scripts / Script Manager、承認、Build/Reload、診断、Docs、設定 |

**互換性上の注意:** 旧PluginはHost.size == 96を要求する。単にHost末尾を追加してsizeofを渡すと壊れる。
旧API 100には旧sizeのprefixを提示し、旧Pluginのentryには64 bytesを渡す必要がある。
新APIは末尾の関数の存在をoffsetof/sizeで検査し、既存prefixとABI 1を維持する。

## 実在するGlassの入口

1. `include/medicon/gui/LiquidGlass.hpp`:
   `void publish_liquid_glass(const LiquidGlassFrame&)`、`const LiquidGlassFrame* current_liquid_glass()`、
   `void forget_liquid_glass(ImGuiContext*)`。
   `LiquidGlassFrame`はcenter、size、scale、waist、lens、energy、light、opacityのコピー可能な値。
2. `src/gui/BlockCount.cpp`, `BlockCount::render(GuiContext&)`が値を作ってpublishする。
3. `include/medicon/gui/EdgeLit.hpp`, `src/gui/EdgeLit.cpp`:
   `bool EdgeLitD3D11::render(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, ID3D11RenderTargetView*, std::string&)`。
   既存ArrayListエフェクトの後に同じ所有者の`LiquidGlassD3D11 glass_`へ転送する。
4. `include/medicon/gui/LiquidGlass.hpp`, `src/gui/LiquidGlass.cpp`:
   `bool LiquidGlassD3D11::render(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, ID3D11RenderTargetView*, const LiquidGlassFrame&, std::string&)`。
5. Main Menu / Social / Alt / Watermarkは、別の軽量な
   `float draw_glass_plate(ImDrawList*, ImVec2, ImVec2, float radius, const GlassPlate&)`
   (`include/medicon/gui/Glass.hpp`, `src/gui/Glass.cpp`)を使う。
   呼出例: `src/gui/MainMenu.cpp`、`SocialChat.cpp`、`SocialWidgets.cpp`、`SocialScreen.cpp`、`AltScreen.cpp`、`Watermark.cpp`。
   このプレートに屈折/背景blurはない。ClickGUI本体も矩形の通常UIで、液体ガラスのGPU入口ではない。
6. Toastは`src/gui/Toasts.cpp`の`ToastManager::render(...)`。LiquidGlassのcallsiteではない。

## GPU / phase / ownership

- `src/platform/OverlayD3D11.cpp`は既存`EdgeLitD3D11`を使い、ImGui描画より前にエフェクトを処理する。
- `src/platform/OverlayD3D12.cpp`もD3D11On12の既存interop deviceとwrapped resourceで同じエフェクトを処理する。
- `src/preview/Main.cpp`も同じ`EdgeLitD3D11`。Script専用のdevice/rendererは不要。
- `LiquidGlass.cpp`のpublishはImGui contextとframe numberに紐づくthread_localデータ。変更前は1枚を上書きする方式。
- `LiquidGlassD3D11::Impl`がdeferred context、capture、half-size blur surfaces、定数バッファ、shaderを所有する。
  shaderは同ファイル内の既存HLSLをプロセス中一度だけコンパイルする。
- 背景は現在のゲームscene（既存EdgeLitの後、ImGui全体の前）。ImGuiのMain Menuや別ウィンドウの文字を背面として再取得する経路ではない。
- ROIを64px単位へ切り上げ、背面をCopySubresourceRegion、half-sizeへdownsample、既存blurを2組、既存GlassPSを合成する。
  scissorはROI。ExecuteCommandList(..., TRUE)で変更した状態を復元し、backbufferを保持しない。
- reset()、device変更、ResizeBuffers時のhostリソース破棄/再作成を既存hostが担当する。
- 複数paneの対応には既存publisher/renderer内のbatch拡張が必要。背景を一度だけ確保して同じ画像から全paneを描く方針。
  新しいshader、captureクラス、blur pass、GPU cache、Script用rendererは追加しない。

## 実パラメータと未対応

| 項目 | 調査時の実装 |
|---|---|
| 座標/サイズ | 物理px。scaleは1080p基準のHUD倍率 |
| 角丸 | `Impl::render`がmin(halfWidth, halfHeight)を`p.form[0]`へ設定。既存shaderは角丸uniformを使うが、Frameには独立フィールドがない |
| refraction | `lens`を[0,1]へclamp。屈折と吸収の両方に作用する。独立した屈折だけのノブではない |
| opacity | [0,1]へclamp、premultiplied合成 |
| waist | BlockCount animatorの形状変形。公開Scriptノブにしない |
| light / energy | lightはradians、energyは[0,1]。BlockCountアニメーション由来 |
| blur | half-size density測定用の固定処理。Scriptに指定できるblur強度はない |
| tint / saturation | shader内の固定値。外部指定するuniformはない |
| border / highlight / shadow | shader内の固定計算。独立した既存設定はない |
| presets / quality | LiquidGlassにはclear/frosted/prominentや品質tierの既存設定はない |
| fallback | 既存`draw_glass_plate`がradius/fill/fill_opacity/alpha/dropを持つ。読みやすいフォールバックへ再利用可能 |

## 再利用する非描画機能

- Widget/input: 既存ClickGUIのImGui (`src/gui/ClickGui.cpp`)とSocialの`SocialInput`/`SocialMotion` (`SocialWidgets.hpp`)。
  ScriptのUIにはImGuiのwindow/focus/button/progressを使い、GUIが閉じたときに入力を受けない。
- Theme: `include/medicon/gui/Theme.hpp`のprimary_text/secondary_text/accent/panel_background/borderとtheme::glass。
- Timing: `include/medicon/gui/Clock.hpp`の`FrameClock`はsteady_clock/QPC、dt上限0.1秒。SDK用の絶対時刻も同じsteady_clock系を使う。
- Notifications: `SocialUi::notify(Toast, FontManager&, bool in_menu, bool enabled)`はprivate。
  glyph登録、`ToastManager::push`、`SocialStore::notify().in_game`/sound、500ms音制限を持つ。薄い公開入口を追加してここへ転送できる。
- Sound: `social::play_notification_sound() noexcept` (`src/social/Sound.cpp`)は既存の1種類のchime。
  Click/Error別音色は存在しないため独立対応を宣言できない。
- Event: 本体`EventBus`と現在のScript Tick/Render/Keyを維持し、コピー済みworld/screen状態から参加/離脱/resizeイベントを生成できる。
- Storage: ConfigStoreは設定用。Scriptの任意文字列KVや世代に紐づくTimerサービスは未実装であり、新たな非GPUサービスが必要。
- Accessibility: 調査範囲に既存のGlass品質tier、Reduce Transparency、global Reduce Motion、High Contrastスイッチはない。
  型付きAPIを用意しても「既存設定がある」と記載しない。追加するならOSの設定を読む共通host policyとし、Script専用プロフィールは作らない。

詳細な実装後の対応表は`docs/19-existing-liquid-glass-bridge.md`、検証結果は受け入れテスト文書へ記載する。
