# Medicon Main Menu: 実装・依存・検証

更新: 2026-10-26（日本時間）。独自Main Menuの表示とボタン反応、ASLR判定修正後のPlay / Settingsの画面遷移をユーザーが実ゲームで確認。追加IDA資料に基づき、純正controller更新Hookから既存ボタン処理へ接続。

## 結果

| 項目 | 実装・根拠・検証範囲 |
| --- | --- |
| Menu判定 | `sdk::BedrockMenu` が現在のprimary ClientInstanceからSceneStack最上位を読み、検証済みUISceneの画面IDが `start_screen` の場合だけ有効化。実行ファイル1.26.52.3と観測したコードを照合。未知の画面型・読み取り失敗・版違いは無効化してログを記録。詳細は [SDK検証記録](main-menu-sdk-validation.md)。ゲーム関数やゲームの文字列アロケータは呼ばない。 |
| 純正UI非表示 | 既存DXGI Present / Present1、ImGui、D3D11 / D3D12による最前面の不透明な全面オーバーレイ。`ScreenView_setupAndRender` のスキップ安全性が未確認なので、純正描画のスキップhookは追加しない。 |
| 入力 | 既存WndProc・入力キュー・MouseCaptureController・native grab抑止を再利用。最新対策ではOSカーソルを表示し、位置更新をウィンドウスレッドで処理してPresent周期による見た目のカクつきを抑える。キー、文字/IME、マウス、raw keyboard / mouseを遮断。ウィンドウ内のカーソル固定は解除し、raw no-legacyの相対移動も仮想デスクトップ全体へ反映する。ウィンドウ外で始まるRaw Inputのクリック・ホイールはGUIに転送しない。フォーカス喪失・退場・ウィンドウ移動／サイズ変更で相対座標をリセット。メニュー世代が異なる入力は破棄し、押下後のrepeat/upも新画面へ通さない。 |
| 背景 | 不透明 `#D9F2FF` と5個の放射グラデーションメッシュ。使用色は指定3色だけ。中心α0.8、外周α0、減衰は `0.8*(1-r²)²`。既存ImGui backendの通常source-over。加算なし。 |
| ロゴ | 上部中央のMedicon。既存DynaPuff Medium。薄ピンクと白の周期グラデーションを頂点色で表現し、`x/波長 + t/3.6` の位相で右から左へ流す。水色の薄い縁取り。D2D等の別描画方式やブラシは追加しない。 |
| ボタン | 左余白を取った縦3列のPlay / Settings / Quit。同幅・同高・同間隔、半透明の白、角丸、ホバー。押下と解放が同じボタンで成立した場合だけ要求を1件記録し、350msの連打抑制。 |
| コールバック | 描画中はenum要求の記録だけ。PostMessage先の `VanillaNavigation::openPlay/openSettings/quitGame` はAction+世代をローカルキューへ入れる。専用の純正MainMenu controller更新Hookでoriginalを呼んだ後、確認済みcaller・現在画面・所有context・世代・コード等を検証し、既存の登録済みhandlerを呼ぶ。DirectX Hook内からネイティブ遷移は呼ばない。[接続詳細](main-menu-native-navigation.md)。 |
| Build | Windows x64 Releaseビルド成功。全体CTest 20/20成功（79.22秒）。ASLR判定修正後は関連6/6成功（18.72秒）。 |
| Runtime | 実ゲーム1.26.52.3でMain Menu表示とボタン反応、修正版でのPlay / Settings遷移をユーザー確認。Quit確認取消／Yes、画面復帰、カーソルの滑らかさの個別確認は未報告。 |

## ブロブの固定パラメータ

半径は `min(W,H)` に対する比率。主周期Tx/Tyは6〜12秒で分散。
位置は指定されたsin/cosの2項合成で、追加項の周期はTx×1.37、Ty×1.61。
位相は各軸・各項で別々。振幅は正規化した画面座標でRMS速度6.5%/秒となるよう計算。

| ブロブ | 色 | 半径 | Tx | Ty | 600秒サンプルの平均速度 |
| --- | --- | --- | --- | --- | --- |
| 1 | 薄ピンク | 0.67 | 6.8秒 | 8.3秒 | 6.17124%/秒 |
| 2 | 水色 | 0.62 | 9.1秒 | 10.7秒 | 6.15955%/秒 |
| 3 | 薄水色 | 0.70 | 11.4秒 | 7.3秒 | 6.12624%/秒 |
| 4 | 薄ピンク | 0.55 | 8.0秒 | 11.8秒 | 6.12305%/秒 |
| 5 | 水色 | 0.59 | 10.3秒 | 9.6秒 | 6.16048%/秒 |

速度の単位は `sqrt((vx/W)²+(vy/H)²)`。瞬間速度はsin/cosの折り返しで変わる。
時間は `steady_clock` の経過実時間。フレーム差分の積算には依存しない。
各放射メッシュは96分割×32リング。方向ベクトルを再利用し、新しいGPU資源は保持しない。
ロゴのフォントは必要7文字だけを格納し、メニュー非表示中は追加フォントを生成しない。
デバイスロスト・リサイズ時は既存backendとatlasの再作成経路を使用。

## 使用したSIG / SDK

- 指定された [surukotonai/solstice](https://github.com/surukotonai/solstice) のコミット `1d7808f52ef4ba6e34a7fbcc65294907a7761611`。ローカル参照とリモートHEADを照合。
- 既存 `BedrockChat` の `MinecraftGame_instance` signature走査・primary client検証を共有。
- solsticeの `ClientInstance::getScreenName` / slot246を調査の出発点とした。1.26.0の注記だけを根拠に採用せず、手元の1.26.52.3の該当関数・SceneStack・UISceneの経路を限定して検証。
- 既存 `ScreenView_setupAndRender` signatureはオフラインでUISceneを特定する根拠として使用。新たなゲーム描画hookは設置していない。
- 追加の推測SIG、未確認のoffset、LeviLamina / bedrockdataの互換性仮定は使用していない。

## 不足している定義（ユーザーによる取得が必要）

追加資料の既存callable配置と純正update callsiteを実ファイルと照合し、厳密に一致する経路へ接続した。
現在利用する経路の静的定義に追加取得待ちはない。実ゲームでのPlay / Settings遷移はユーザー確認済み。未確認の操作は下記に区別する。
一般のScreenEvent完全型・native FIFO・低レベルquit候補の型は未取得だが、現在の実装では利用しない。
接続前の追加依頼は [純正遷移の不足一覧](main-menu-navigation-missing.md) に履歴として残す。

| 不足 | 必要な内容 |
| --- | --- |
| 未確認の実動作 | 各画面からの復帰、認証・popup、Quit確認取消／Yes、リサイズ・デバイスロスト等の実ゲーム検証。未知のruntime配置では呼ばずに理由をログへ記録する。 |
| 追加の画面型・版 | 1.26.52.3以外のrevision、未知のScene / controller、別SceneStackルートが必要な場合はその検証済み定義。現在は安全のため無効化。 |

共通の検証済み遷移APIで3操作を行えるなら3本の別sigは不要。
`playUi`（音再生）やコンテナ画面のexitは代用しない。
純正MainMenuの既存handlerを使い、独自の文字列・event・native queueを構築しない。

## 検証と再現方法

- 新しいMainMenuテスト: 放射メッシュ、不透明背景、色、周期・速度、時刻依存性、左向きロゴ、リサイズ配置、押下・解放・フォーカス・連打・要求キューを検証。
- runtimeテスト: メニュー中の入力所有、Insert/Esc/モジュールキーの遮断、入退場・再入場・停止・再起動の状態を検証。
- 既存のD3D11 / D3D12 / Present1別スレッド入力ホスト、GUI、DLL lifecycle等も実行。これらの独立ホストはMinecraft互換性の実動作確認ではない。
- プレビューPNGを1280×720（0秒）、1920×1080（1.5秒）、720×1280（4秒）で生成し、全画面被覆・配置・描画を目視確認。
- Main Menu表示を伴うPreviewのD3D11デバイス再作成テストも成功。実ゲームのデバイスロストを再現した試験ではない。
- Play / Settingsの遷移はユーザー確認済み。ワールド参加・各画面からの復帰、最新カーソル修正、フルスクリーン切り替えは個別未確認。

```powershell
./tools/build.ps1 -Configuration Release
./out/build/local/bin/MediconPreview.exe --main-menu
./out/build/local/bin/MediconPreview.exe --main-menu-time 1.5 --size 1920x1080 --screenshot out/main-menu/menu.png
```

`--main-menu` / `--main-menu-time` は単独Previewだけの明示モード。DLLの画面判定を上書きする設定はない。
診断と押下ログは `%LOCALAPPDATA%/Medicon/inject.log`。版・コード・画面の解決に失敗した場合も理由を記録。
