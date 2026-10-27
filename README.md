<<<<<<< Updated upstream
メジコンod player専用client.wtf
=======
# Medicon

Minecraft Bedrock Edition向けクライアントの土台。Windows x64 / C++20 / CMakeで構成しています。

モジュールと設定の登録を共通化し、機能を追加するたびにGUIや保存処理を書かずに済むようにしています。`Minecraft.Windows.exe` にDLLを読み込むと、自動でDXGI描画・ウィンドウ入力Hookを接続し、約2秒の起動アニメーションを表示します。ClickGUIは割り当てキーを押すと開きます。ゲーム内チャットのbindコマンドに必要な最小SDKを実装し、その他のゲーム内機能は次の実装段階です。

## 実装済み

- `Module` / `ModuleManager`: 有効化・無効化、キー割り当て、フレーム更新、描画コールバック。
- `Setting`: bool、整数、小数、選択肢、RGBA、文字列。GUI自動生成とJSON保存に対応。
- `ClickGui`: 紫→赤のアイコン付きヘッダー、四角いDropdownパネル、ONの行全体のハイライト、行内の設定展開。Module切り替え、キー割り当て、独自パネルにも対応。
- `ArrayList`: 右端のピンク→バイオレットのチューブと暗いすりガラスのHUD。DynaPuff Medium書体、ガウスぼかし、ON/OFFパルスと幅順の滑らかな並び替え（[Edge-lit仕様・検証](docs/EDGELIT.md)、[設定とプレビュー](docs/gui.md)）。
- `ConfigStore`: スキーマ検証後にまとめて読み込み、一時ファイル経由で保存。壊れた設定を自動で上書きしません。
- `EventBus`: 型別イベント、RAIIによる購読解除。単一スレッド用。
- sig基盤: Solsticeの固定コミットから63件を収録。ワイルドカード、一意性チェック、rel32、PE実行セクションの走査。
- `HookManager`: MinHookの管理、Hookの有効化・解除・失敗時のロールバック。
- `CommandManager` / `sdk::BedrockChat`: Minecraft本来のチャット入力によるローカルbindコマンド、即時保存、ゲーム内チャットへの結果通知。
- `Medicon.dll`: Minecraftでの自動起動、D3D11／D3D12のPresent／Present1描画、リサイズ時のGPU資源再作成、Insert開閉と入力の接続。従来のホストImGui向けC APIも使用できます。
- `MediconPreview.exe`: MinecraftなしでGUIと設定を確認するDirect3D 11アプリ。
- Social: メインメニュー下端の Social ボタンから開く画面です。`social-server` とWinHTTPで接続し、サインイン、ニュースと Change Log、Global チャット、フレンド、DM（日本語 IME 対応）、通知設定、通知ポップアップを扱います。設定は`%APPDATA%\Medicon\social.json`、ライセンスはDPAPIで保存します。[構成、Preview、実サーバーでの確認、現在の制限](docs/social.md)。
- `MainMenu`: 1.26.52.3の検証済み画面情報を読めた場合だけ、不透明なパステル背景・流れるMediconロゴ・左揃えボタンを既存描画基盤で表示。Play / Settings / Quitは要求をキューへ入れ、確認済みの純正controller更新Hookから既存ボタン処理へ接続します。Play / Settings遷移はユーザーが実ゲームで確認。[純正遷移の接続・検証範囲](docs/main-menu-native-navigation.md)。

## ビルドと起動

この作業環境には、検証用のLLVM MinGW / CMake / Ninjaを`.tools`内に配置しています。システム全体の設定変更はしていません。

```powershell
./tools/build.ps1 -Configuration Release
./out/build/local/bin/MediconPreview.exe
```

`Insert`でGUIの開閉。GUI内のキー割り当てで`Esc`は中止、`Backspace`は解除です。設定の保存先は通常`%LOCALAPPDATA%\Medicon\config.json`。プレビューとDLLは同じスキーマを使います。

### Bindコマンド

Minecraft 1.26.52系が対象です。ワールドに参加し、ClickGUIを閉じてMinecraft本来のチャット欄を開き、`.bind clickgui rshift`と入力してEnterを押します。接続できている場合、ローカルコマンドはサーバーへ送る前に止め、通常のチャットはそのまま送信します。結果はキューを通じてローカルプレイヤーのBaseTickからゲームのGuiDataへ渡し、Minecraftのチャットに表示します。

コマンドの結果・エラー本文はMinecraft純正チャットだけに表示し、ClickGUIのステータスや独自の通知ウィンドウには表示しません。純正チャットへの通知が一時的にできない場合はキューに残して再試行します。

変更したキーはその場でconfig.jsonへ保存します。壊れた設定ファイルは上書きせず、そのセッションだけの変更になったことを結果に表示します。接続・実行・保存・結果通知の診断は`%LOCALAPPDATA%\Medicon\inject.log`で確認できます。

```text
.bind clickgui rshift       # GUIを右Shiftに割り当て
.bind example r             # ExampleモジュールをRに割り当て
.bind "ClickGUI" f6         # 表示名でも指定可能（空白入りの名前は引用符で囲む）
.bind clickgui              # 現在の割り当てを表示
.bind                      # 割り当て一覧
.unbind example            # 割り当て解除（.bind example none も同じ）
.bind reset                # 他の割り当てを解除し、ClickGUIをInsertに戻す
```

上の`#`以降は説明です。入力時には含めません。モジュールID／表示名とキー名は大文字・小文字を区別しません。英字・数字、F1〜F24、Insert、Space、Tab、Esc、LShift／RShift、Ctrl、LCtrl／RCtrl、Mouse4／Mouse5などに対応します。F8も通常の割り当てキーとして使用できます。ClickGUIの割り当てを解除するとInsertに戻り、GUIへ戻れなくなるのを防ぎます。設定済みの割り当ては`.bind list`でも確認できます。

チャット接続は[Solsticeの送信前TextPacket処理](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/Hook/Hooks/NetworkHooks/PacketSendHook.cpp)を参考に、`src/sdk/BedrockChat.cpp`で最小限の読み取り・呼び出しを実装しています。実行ファイルの版、signatureの一意性、ClientInstance／packet senderのvtableを検証して接続します。ゲームのMSVC STLをMediconのMinGW STLへキャストせず、結果通知には呼び出し中だけ有効なPOD string envelopeを渡します。対象外の版や接続を検証できない場合はチャットHookを設置せず、理由を`inject.log`へ記録します。詳細は[docs/commands.md](docs/commands.md)を参照してください。

1.26.52.3では、実プロセスを読み取る検証でprimary ClientInstanceとpacket senderの取得経路を確認しています。結果通知のsignatureと入力文字列のコピー動作は実行ファイルのオフライン解析で確認し、Minecraft純正チャットへの出力は実ゲームでも動作確認済みです。

Minecraftへ読み込むDLLは `out/build/local/bin/Medicon.dll` です。通常のx64 `LoadLibrary` による読み込み後、loader lockの外で自動起動します。最初の描画フレームから、動くパステル背景・kawai.otfの「メジコン」・mez.pngの起動アニメーションを約2秒間表示します。終了後もClickGUIは閉じたままで、割り当てキー（初期値は`Insert`）を押すと開きます。前回の終了時に開いていても、inject時には自動で開きません。GUI表示中はゲームへのウィンドウキー／マウス入力とカーソル固定・移動を抑止し、GUI側にカーソルを描きます。起動状況や失敗の詳細は `%LOCALAPPDATA%\Medicon\inject.log` に追記します。

起動アニメーションの時間は`include/medicon/gui/InjectSplash.hpp`の`SLIDE_IN_DURATION`（0.70秒）、`HOLD_DURATION`（0.90秒）、`COLLAPSE_DURATION`（0.40秒）で調整できます。`MediconPreview.exe --splash-time 1.30 --size 1280x720 --screenshot splash.png`で、指定した経過秒の画面を保存できます。

タイトル画面の判定に必要なゲーム内部の特定（signature走査・メニュー/遷移のプロファイル確認）は、描画Hookを入れる前に済ませます。そのため起動アニメーションの最初のフレームからタイトル画面を判定でき、検出済みのメニューはアニメーションの下に描かれます。収縮でMediconのメニューが現れ、Minecraft本来のタイトル画面は見えません。アニメーション中のメニューはクリックを受け付けず、終了後にボタンを離した状態を一度確認してから操作できます。

メニューのロゴ・ボタン用フォントは起動アニメーションの最初のフレームより前にまとめて生成・転送します。タイトル画面の検出が遅れても、アニメーション終了時にフォントアトラスを作り直しません（ウィンドウサイズや使用文字が変わらない場合）。

ClickGUIを開くとMinecraftの`ClientInstance::releaseMouse`を呼び、開いている間の再grabを止めます。閉じると、開く前にゲームがマウスを取得していた場合だけ`grabMouse`で戻します。タイトル画面など元からマウスが解放されている場合は強制取得しません。これらの呼び出しはゲームのウィンドウスレッドで行い、Alt+Tab中には再取得しません。カーソル位置は画面座標から毎フレーム更新します。ネイティブ接続は1.26.52系の版・vtableとマウス状態のコードを検証して使用し、診断は同じ`inject.log`へ記録します。

D3D12では、SwapChain生成時のコマンドキューを記録します。生成後に読み込んだ場合は、同じデバイスのDirectキューを描画処理から検出します。キューが複数あり特定できない場合は描画を待機します。別のDXGIアプリでの自動描画検証には `include/medicon/platform/OverlayApi.h` の `MediconStartOverlay()` を明示的に呼びます。`MediconOverlayStatus()` と `MediconOverlayLastError()` で状態と診断を取得できます。

自動描画を止める場合は、描画・入力コールバックとDllMainの外で `MediconStopOverlay()` を呼びます。Hookとウィンドウ入力を復元しGPU資源を解放します。遅れて到着するコールバックや他のツールのウィンドウプロシージャ連鎖を保護するため、起動後のDLL本体はプロセス終了までメモリに保持します。DLLを更新して再試行するときはMinecraftを再起動してください。インジェクター本体とmanual map用の起動処理は含めていません。

別のPCではVisual Studio 2022の「C++によるデスクトップ開発」とCMake 3.25以上を用意し、次の手順でビルドできます。初回のCMake設定時は依存関係の取得にネット接続が必要です。

```powershell
cmake --preset windows
cmake --build --preset release
ctest --preset release
./out/build/windows/bin/Release/MediconPreview.exe
```

`.tools`、`.reference`、`out`はGit管理対象外です。依存関係の版はCMakeでコミット単位に固定しています。`-DMEDICON_BUILD_WINDOWS=OFF`を指定すると、描画に依存しないcore／sig部分のみビルドできます。

## Moduleを追加する

`include/medicon/modules`と`src/modules`にクラスを追加します。最小例:

```cpp
#include <medicon/core/Module.hpp>

class MyModule final : public medicon::Module {
public:
    MyModule()
        : Module("my_module", "My Module", "Example module", medicon::Category::Visual),
          size_(add_setting({"size", "Size", "Display size", 1.0F, 0.5F, 3.0F, 0.1F})) {}

protected:
    void on_enable() noexcept override {}
    void on_disable() noexcept override {}
    void on_tick(float delta_seconds) noexcept override {
        const float size = size_.get<float>();
        (void)size;
        (void)delta_seconds;
        // 有効な間だけ呼ばれる更新処理。
    }
    void on_render() noexcept override {
        // ホストのImGuiフレーム内。有効な間だけ呼ばれる。
    }

private:
    medicon::Setting& size_;
};
```

`src/modules/BuiltinModules.cpp`でヘッダーをincludeし、登録を1行追加します。

```cpp
modules.add<MyModule>();
```

`src/modules`内の`.cpp`はCMakeが自動検出します。GUIの一覧・設定部品・保存と復元には追加コードが不要です。全設定型の例は`ExampleModule`にあります。Module／Settingの`id`は保存キーなので、公開後は安易に変更しないでください。数値設定には範囲を指定します。Moduleの登録は初期化時に行い、コールバックから一覧を変更しないでください。

## GUIを追加する

既存の設定UIを使う場合は`add_setting()`だけで十分です。独自ウィンドウは`gui.add_panel("Debug", callback)`、Module固有の設定UIは`gui.set_module_settings_renderer(id, callback)`で追加できます。

詳しい例は[docs/gui.md](docs/gui.md)を参照してください。

## ゲームとの接続

描画バックエンドとゲームのABIをcoreから分離しています。`src/platform/AutoOverlay.cpp` がMinecraftへの自動接続を担当し、`OverlayD3D11.cpp`／`OverlayD3D12.cpp` が専用ImGuiコンテキストで描画します。`src/preview/Main.cpp` は単独D3D11アプリの実例です。自動接続と以下のホスト向けC APIは、それぞれ独立したClient／ImGuiコンテキストを使います。

DLLへ外部から接続する場合は、`include/medicon/platform/ClientApi.h`のC APIを使用します。

1. `LoadLibrary`後、loader lockの外で`MediconInitialize(config_path)`。
2. ホストのImGuiが同じ1.91.9bであることを`MediconImGuiVersion()`と比較。
3. ホストの`ImGui::GetAllocatorFunctions()`の結果を`MediconSetImGuiAllocators()`へ渡す。
4. 各フレームでプラットフォームbackendの`NewFrame()`後、`ImGui::NewFrame()`前に`MediconPrepareFrame(context)`。戻り値が2ならフォントテクスチャを再作成（例: `ImGui_ImplDX11_InvalidateDeviceObjects()`→`CreateDeviceObjects()`）。
5. ホストの`ImGui::NewFrame()`後に`MediconFrame(context, delta_seconds)`、続いて`ImGui::Render()`。D3D11では完成したゲーム背景のTexture2DとRTVを`MediconRenderEdgeLitD3D11(context, device, immediateContext, backBuffer, renderTarget)`へ渡してHUDを重ね、最後にホストのImGui描画を行う。自動MinecraftオーバーレイとPreviewはこの描画パスを内蔵。
6. ホストからキーイベントを`MediconKeyEvent()`へ渡し、戻り値1のキーはゲームへ渡さない（GUI表示中のEscを含む）。Mouse4/5は`VK_XBUTTON1/2`で渡せる。ImGuiへの入力（マウス）もホスト側で処理し、GUI表示中はゲームへのマウス入力を止めてカーソルを表示する。Edge-litは自身のオフスクリーンGPU資源を持ち、ホストの描画状態を復元する。ホスト側のResize・Device Lost処理とバックバッファ管理は従来どおりホストが行う。
7. コールバック配信を止めて実行中の処理が完了してから、`MediconShutdown()`、`FreeLibrary`の順に終了。

PrepareFrame・Frame・KeyEvent・ResetKeys・Shutdownは同じUIスレッドで呼びます。フォーカス喪失時は`MediconResetKeys()`を呼んでください。ホスト向けC APIの診断は`MediconLastError()`で取得できます。Minecraftの自動接続ではウィンドウスレッドの入力をキューに入れ、Present側のスレッドでGUIとClientを操作します。

`Client::scan_signatures()`は明示的な走査です。ゲームの実行ファイル内で呼び、`client.signatures().address(name)`で結果を確認できます。走査成功はゲームABIの互換性を保証しません。Solsticeのコメントには複数の版が混在しており、収録signature全体のMinecraftでの互換性は未検証です。出典・更新方法は[docs/signatures.md](docs/signatures.md)を参照してください。

## 構成

```text
include/medicon/
  client/       全体のライフサイクル
  core/         Module、Setting、ConfigStore、EventBus
  gui/          自動設定GUIと拡張点
  hooks/        MinHookの管理
  memory/       sig解析・解決・実行ファイルのメモリ範囲
  modules/      機能の追加先
  platform/     DLLの公開C API
  commands/     ローカルbindコマンドの解析と設定変更
  sdk/          Minecraftのチャット・メニュー・ゲームプレイ接続
  social/       Socialの通信形式・設定・状態とネットワークサービス
src/            各実装とpreviewホスト
data/signatures/ 出典を記録したsigデータ
tests/          core、sig、Hook、GUIフレーム、DLLライフサイクル
tools/          ビルドとsig再取得
```

## 検証

CTestではcore、sig、非表示D3D11プレビュー、runtime、DLLライフサイクル、GUI、プレビュー入力に加え、D3D11／D3D12の独立描画ホストで自動接続を検証します。DLLを描画開始前／開始後に読み込み、実際のバックバッファのGUIピクセル、Insert開閉、リサイズ後の再描画、停止後の元の描画を確認します。独立ホストでの成功だけではMinecraft本体の全バージョンとの互換性は保証しません。

コマンドの解析・即時保存・C APIと、制御されたTextPacket fixtureでの送信前キャンセル／通常チャット通過も検証対象です。これらは実ゲームでチャットを入力する一連の動作確認とは区別しています。

依存関係と出典は[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)に記載しています。

## ゲーム用module

ClickGUIからChinaHat、Derp、Aimbot、Killaura、Scaffold、ESPを有効にできます。初期状態はすべてOFFです。ChinaHatとESPはVisual、Derp・Aimbot・KillauraはPlayer、ScaffoldはMovementにあります。

ESPは枠の外側にGlowを描画し、色・幅・強さを変更できます。ChinaHatは三人称視点で自分の頭上に表示します。Aimbotの初期設定は攻撃ボタンを押している間だけ動作します。Scaffoldは足元から移動方向へ設置候補を探し、隣接ブロックの面を使って設置します。

ゲーム接続はMinecraft Windows `1.26.52.3`向けです。ゲーム操作はlocal playerのbaseTickに集約し、ClickGUI・メニュー・フォーカス喪失中は停止します。詳細と検証範囲は[docs/gameplay-modules.md](docs/gameplay-modules.md)を参照してください。

GUI/UIの性能改善、変更前後の計測値、再計測手順は[docs/ui-performance.md](docs/ui-performance.md)を参照してください。
>>>>>>> Stashed changes
