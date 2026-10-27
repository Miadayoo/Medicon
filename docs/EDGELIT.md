# ArrayList Edge-lit

2026-10-25 の実装仕様。数値は画面の物理 px。ClickGUI の自動スケールや Minecraft の GUI スケールとは独立し、以下の寸法に ArrayList の `scale` だけを掛ける。

## 設定とレイアウト

`modules.arraylist.settings` の公開項目は6個。ArrayList 自体の有効・無効は従来どおり Module の `enabled` で指定する。旧 ArrayList の背景・位置・文字色等の設定は新しい描画には使わない。

| ID | 既定値 | 範囲 |
|---|---|---|
| `accentA` | `#FF4FA0` | RGB |
| `accentB` | `#8B6BFF` | RGB |
| `glow` | 1 | 0–1.6 |
| `shadow` | 1 | 0–1.6 |
| `pulse` | true | bool |
| `scale` | 1 | 0.5–2 |

上端8、チューブ幅3・右端余白6、行高24・行間0。プレートの右端は `W−9`。幅は `10 + nameAdvance + (suffix があれば 7 + modeAdvance) + 12`。実描画と同じフォントで計測して幅の降順に並べ、同じ幅は登録順を維持する。`show_in_arraylist()` と `Module::suffix()` の既存の選択規則を使う。

名前は同梱 Geist SemiBold 600・15px em、suffix は Geist Mono Medium 500・12px em。各サイズで直接ラスタライズし、それぞれを行の縦中央へ配置する。移動中の座標は小数のまま、停止した文字のベースラインだけを整数に合わせる。角丸は半径5、先頭の左上と、次の行が短い場合または最終行の左下だけ。

行色は先頭と末尾の行中心の間で A→B を sRGB チャンネルごとに線形補間。1行ならA。待機中に色相・輝度を変化させない。

## 描画

`ArrayList.cpp` がフレームの配置・文字を `EdgeLitFrame` に発行し、`EdgeLitD3D11` がゲームの完成フレームへ描く。D3D12 は同じデバイスとキューの D3D11On12 ラップを使い、同じシェーダーを実行する。ImGui の ClickGUI はこの後に描く。

全合成は premultiplied source-over（`ONE`, `INV_SRC_ALPHA`）。ぼかしは専用オフスクリーンテクスチャで水平・垂直の2パスガウス畳み込み。半径 `ceil(3σ)`、重みは実際のσから計算して正規化する。σ≥8は半解像度でσ/2、復元はバイリニア。影をずらした矩形の重ね描きや、Kawase・box blur では代用しない。

| 順 | レイヤー | 仕様（scale前） |
|---|---|---|
| 1 | 影 | 行矩形を各辺3拡張した和集合、左8・下3、σ11、`#02030A` α.62×shadow |
| 2 | ブルーム | `W−25..W+15`, 上下16拡張、σ16、透明A→A(15%)→B(85%)→透明B、α.42×glow |
| 3 | プレート | **1・2の描画後**の毎フレームの背景をσ10でぼかし、彩度1.3倍。地 `(10,11,17)` α.66、その上へ右端から照り返し |
| 4 | 文字 | 左1・下1の接地影σ.5、名前α.55／モードα.5×shadow。モードの色cの発光σ3.5 |
| 5 | チューブ | グロー幅9・上下3・角丸5・σ4・α.95×glow、本体幅3・角丸2・不透明A→B、白い芯幅1・上下2・α.85→.55 |
| 6 | パルス | 19×40・角丸10・σ6・mix(c,white,.35)・α.9×glow×L、白い芯3×28の透明→白→透明グラデーション |

背景はプレート範囲にガウスの余白を付けて切り出し、ぼかしてから角丸SDFで切り抜く。影マスクは重なる行のαを足さず和集合を作る。背景テクスチャは毎フレーム更新する。GPU資源の生成失敗は描画バックエンドのエラーとして報告する。

照り返しは右端α.36（点灯時.60）、左38でその.35倍、左104で0。名前はmix(c,white,.88→.78)、モードはmix(c,white,.38→.68)、モードの発光α.60→.90。矢印の両端はL=0→1で、その間は線形補間する。すべての光のαへglow、接地影を含む影のαへshadowを掛け、1でクランプする。

## 動き

- ON：高さ200ms、右からのスライド240ms、文字60ms遅延＋160ms。すべてeaseOutCubic。横移動距離は行幅＋12。
- パルス：チューブ上端から行中心へ220ms easeOutCubic。到着でL=1、1200msでeaseOutCubicに従い0へ戻る。
- OFF：パルス到着でL=.6、右スライドとフェード200ms easeInCubic。スライド開始80ms後から高さを200msで畳む。
- 幅・行y・全高：`v += (target−v) × (1−exp(−dt/.060))`。並べ替え中も同じ行の状態を維持する。
- パルスは最大4本、超過時は古いものを外す。連打時も現在の見た目から反転する。pulse=falseでは光のイベントと待ち時間をなくし、行の出入りだけを動かす。

## プレビュー

```powershell
./tools/build.ps1 -Configuration Release
./out/build/local/bin/MediconPreview.exe --demo --size 960x640 --dpi 96 --arraylist-scale 2 --bg day --scenario edgelit_reference --screenshot out/edgelit/day.png
./out/build/local/bin/MediconPreview.exe --demo --size 960x640 --dpi 96 --arraylist-scale 2 --bg night --scenario edgelit_reference --screenshot out/edgelit/night.png
./out/build/local/bin/MediconPreview.exe --demo --size 1920x1080 --dpi 96 --bg white --scenario edgelit_idle --screenshot out/edgelit/white.png
```

`day` / `night` はHUDとは別に描いた検証用のピクセル風景。実ゲームのスクリーンショットではない。`edgelit_reference` は Elytra Fly の点灯、`edgelit_on` / `edgelit_off` / `edgelit_mode` は60・120・220・320・500・1600msの連続画像を出す。`edgelit_no_effects`、`edgelit_no_pulse`、`edgelit_scale`（1.25倍）、`edgelit_single`、`edgelit_empty` で設定と境界条件を確認できる。

独自D3D11ホストでC APIを使う場合は、`MediconPrepareFrame` → `MediconFrame` → `MediconRenderEdgeLitD3D11` → ImGui描画の順にする。`MediconRenderEdgeLitD3D11` へそのフレームの完成背景テクスチャとRTVを渡す。自動MinecraftオーバーレイとPreviewはこの接続を内蔵する。

ビルド成果物は `out/build/local/bin/Medicon.dll` と `MediconPreview.exe`。同梱フォントの出典・ライセンスは `assets/fonts` と配布用 `bin/licenses/Geist-OFL.txt` にある。

全比較画像は `./tools/capture_edgelit.ps1`、ピクセル検証は `./tools/verify_edgelit.ps1` で再生成・確認する。添付参考画像に合わせた `day.png` / `night.png` は2倍表示で、通常の1倍の寸法検証は白・黒背景の1920×1080画像を使う。

2026-10-26 の検証結果：Releaseビルド成功、CTest全15件成功（GUI 3,166チェック）。D3D11／D3D12の独立ホストで実際にHUDを有効にし、GUIを閉じたときの連続したチューブ、リサイズ、描画停止・再開、DLLライフサイクルを確認した。画像の23チェックではチューブ3pxとグラデーション、glow／shadowゼロ時の基本形状・文字、影の滑らかな減衰、夜のグロー、空リストの背景一致を確認した。これはPreviewと独立描画ホストでの検証であり、Minecraft実ゲーム内での昼夜の最終確認は未実施。
