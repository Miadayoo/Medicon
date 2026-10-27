# Dropdown ClickGUI / Edge-lit ArrayList — 実装仕様

ClickGUI は 2026-10-26 の参考画像に合わせた、紫から赤のヘッダーと四角いパネルのテーマを使う。操作と保存形式は従来どおり。ArrayList は 2026-10-25 の Edge-lit 仕様へ置き換えた。
ClickGUI の寸法は rpx（1920×1080・UI スケール 1.0 の 1px）、テーマは `include/medicon/gui/Theme.hpp`。
Edge-lit は実画面 px に専用の `scale` だけを掛ける。全レイヤー・描画ホストの説明は [EDGELIT.md](EDGELIT.md) を参照。

## 1. 構成

| 層 | ファイル | 役割 |
|---|---|---|
| Theme / Metrics | `include/medicon/gui/Theme.hpp` | ClickGUI の色・rpx 寸法・時間・影プリセット |
| 数学 | `include/medicon/gui/GuiMath.hpp` | スケール、easeOutCubic、可逆進行度、追従、Slider 値、パネル位置補正 |
| 時計 | `include/medicon/gui/Clock.hpp` | `steady_clock`（QPC）+ 固定 dt。dt は 0〜0.1 秒にクランプ |
| フォント | `src/gui/Fonts.cpp` | ClickGUI は Segoe UI 系、ArrayList は同梱 Geist SemiBold / Geist Mono Medium。実サイズでラスタライズ・計測、CJK フォールバック |
| ClickGUI の影 | `src/gui/Shadow.cpp` | `IShadowRenderer`、解析的メッシュ影（方式 E、§5） |
| 描画補助 | `src/gui/Draw.cpp` | 色変換、HSV、塗りつぶし三角矢印、カテゴリのベクターアイコン、市松模様 |
| ClickGUI | `src/gui/ClickGui.cpp` | パネル、入力、Setting 部品、オーバーレイ、永続化 |
| ArrayList | `src/gui/ArrayList.cpp` / `ArrayListAnimator.cpp` | 表示対象・計測・レイアウト、描画非依存の ON/OFF・パルス状態機械 |
| Edge-lit 描画 | `src/gui/EdgeLit.cpp` | 専用フレームバッファ、ガウスぼかし、すりガラス・チューブ・文字の GPU 描画 |
| ルート | `src/gui/Overlay.cpp` | 上記を束ね、DLL と Preview の両方から同じ順序で呼ばれる |
| 設定保持 Module | `src/modules/ClientModules.cpp` | `ClickGuiModule`（`clickgui`）、`ArrayListModule`（`arraylist`） |

DLL（`Client`）と Preview は同じ `gui::Overlay` と Edge-lit 描画を使う。D3D11 は専用パス、D3D12 は D3D11On12 経由で同じ描画を実行する。ホストの接続・リサイズ・デバイス再作成は `src/platform/*`、プレビューの背景・撮影・デモ Module は `src/preview/*` にある。

フレーム順序（両ホスト共通）:

```
backend NewFrame → Overlay::prepare_frame()（true ならフォントテクスチャ再アップロード）
→ ImGui::NewFrame → modules.render() → Overlay::render()（ArrayList フレーム準備 → パネル → オーバーレイ）
→ ImGui::Render → Edge-lit GPU パス → backend RenderDrawData → Present
```

外部ホストは `MediconPrepareFrame(ctx)`（戻り値 2 = 再アップロード要）で実サイズのフォントを準備し、`MediconRenderEdgeLitD3D11` で完成したシーンへ Edge-lit を重ねる。自動接続と Preview は両方を呼ぶ。

## 2. スケール

ClickGUI: `uiScale = clamp(max(min(W/1920, H/1080), dpi/96) × GUI Scale, 0.75, 3.0)`。
Edge-lit: `scale`（0.5〜2.0）を全座標・余白・文字サイズ・ぼかし σ に掛ける。GUI Scale・DPI・画面解像度からの自動拡大はしない。
DPI は ImGui メインビューポートの HWND に `GetDpiForWindow`（動的取得）、Preview は `--dpi` で上書き。
矩形の辺は「パネル原点 + round(オフセット rpx × s)」で丸める（パネル内部の形が位置に依存しない）。線は `max(1, round(rpx×s))`、角丸半径は丸めない。

## 3. ClickGUI（実装値）

- パネル: 開始 (20,88)、幅 190、間隔 12、ヘッダー 36、Module 行 32、Setting 行 32、下余白 2。区切り線・外枠・角丸は 0、最大高さ = floor(0.80 × クライアント高さ) と画面下端までの残り高さの小さい方。ヘッダーは 20px、Module 名は 16px。
- 展開時の高さ = `header + round((sep + rows + border + pad) × v)`。折りたたみ時はちょうど 36。
- ビューポート = ヘッダー + 区切り線の下 〜 下端 − 枠線 − 下余白。スクロールは viewport 基準で 0〜max にクランプ（縮小時は target と現在値の両方）。
- Chevron: 9×5 の塗りつぶし三角形、中心 = 右端から 10 + 7/2。閉じた状態は下向き、開くと 180°回転して上向き。
- ヘッダー: Visual / Movement / Player / Misc / Client の順に #9916FF / #D51BE8 / #F016B7 / #FF286B / #FF4028。左端はカテゴリ色を 12% 暗く、右端は元の色。
- Module 行: 無効は #363636、有効はカテゴリ色を左端 28%・右端 16% 暗くしたグラデーション。ON/OFF とホバーは補間。行全幅に描画し、アクセント帯は使わない。
- ホバー時キー名: Chevron 枠の左 6 に右寄せ。名前と重なる（間隔 6 未満）なら非表示。
- Setting ブロック: X 0〜190、上 2・下 4、角丸 0、背景 #2D2D2D。ラベル X 11、コントロール終端 X 179。
- Keybind 行: 表示可能な Setting を持つ Module の Setting ブロック末尾に「Keybind [R]」を追加（§逸脱 D3）。
- bool は 12×12 のチェックボックス。Slider はラベル段 18、トラック中心が行下端 −8、高さ 3、カテゴリ色のつまみ 4×12。Dropdown は項目 24・余白 4・最大 8 項目。ColorPicker は余白 8・SV 124・バー 10・間隔 6・16 進 1 行 16。
- GUI 表示中はゲーム背景へ暗いベールと下端の紫・赤の薄いグラデーションを重ねる。GUI 全体のフェードと同期し、背景ぼかし用の追加 GPU パスは使わない。
- 文字列 Setting は読み取り専用表示（初回のみログ）。
- ホイール 1 ノッチ = 54 rpx。Dropdown 内は 1 ノッチ = 1 項目。
- 並び: カテゴリーは enum 順（Visual, Movement, Player, Misc, Client）、Module は登録順。0 件のカテゴリーは非表示。
- Z-order: 押されたパネルを最前面へ（ヘッダー以外の押下も含む）。

### 入力

- 座標はクライアント領域の物理 px（ImGui と同一）。クリック = 同じ要素で押して離し、移動 3px 未満、離した時点で発火。Slider / スクロールバー / ColorPicker は押した時点から。ドラッグ開始は 3px。
- フォーカス喪失（ImGui がボタンを解除し Release が来ない）でドラッグ終了、値と位置は保持。
- DBLCLK は ImGui Win32 backend が DOWN として扱う。ホイールは `delta/120` の浮動小数。
- 他の ImGui ウィンドウ（`add_panel`）がマウスを持つ間は反応しない。
- キー: `Overlay::key_event` が GUI → Module ホットキーの順にルーティング（DLL / Preview 共通）。Esc は 待機キャンセル → オーバーレイを閉じる → GUI を閉じる、GUI 表示中はゲームへ渡さない（`MediconKeyEvent` が 1）。
- 開閉キー = ClickGUI Module の Keybind（既定 Insert）。0 にしても Insert にフォールバック（到達不能防止）。
- Mouse4/5 は ImGui IO または `VK_XBUTTON1/2` のキーイベントで割り当て可能。

### 永続化（既存 ConfigStore、スキーマ変更なし）

`modules.clickgui.settings` に追加: `scale`, `shadow.enabled`, `shadow.opacity`, `shadow.blur`, `shadow.offset_y`（表示）、`panels.<categoryId>.x|y|collapsed`, `panels.order`（非表示 Setting、rpx）。
`modules.arraylist.settings` は `accentA`, `accentB`, `glow`, `shadow`, `pulse`, `scale` の 6 項目のみ（ArrayList 自体の Enabled は Module の enabled）。旧 ArrayList 設定は削除し、位置・文字サイズ・背景・並び順・Suffix の個別設定は公開しない。
保存タイミング: ドラッグ終了、折りたたみ、Slider/ColorPicker を離したとき、その他の変更時、GUI を閉じたとき（要求をフレーム末尾で 1 回にまとめる）。読み込み失敗した Config は自動保存で上書きしない（既存方針）。
`panels.order` に無いカテゴリーは初期配置（20 + i×202, 88）。右端を超える場合は折り返し、段の間隔を最大 540 として画面高に合わせる。複数段では標準位置のパネルを各段の高さに収め、長い一覧をスクロールさせる。旧標準配置だけ新しい幅に合わせて移行し、手動配置を保持する。表示時のクランプ値は書き戻さない。

## 4. ArrayList

Edge-lit の光源は画面右端の 1 本のチューブ。プレート・文字を同じピンク→バイオレットの光で照らす。Module の `enabled` と `show_in_arraylist()` による表示対象は従来どおり。

以下の寸法は `scale = 1` の実画面 px。全値に `scale` を掛ける。

| 項目 | 値 |
|---|---|
| 上端 | 8 |
| チューブ | 幅 3、右端は画面右から 6、全行を連続して覆う |
| 行 | 高さ 24、隙間 0、プレート右端はチューブ左端に接する |
| 行幅 | 左余白 10 + 名前幅 +（モードがあれば隙間 7 + モード幅）+ 右余白 12 |
| 名前 | Geist SemiBold 600、15px em |
| モード | Geist Mono Medium 500、12px em。独立して縦中央へ配置 |
| 並び | 行幅の降順、同幅は登録順 |
| 角丸 | 半径 5。左上は先頭のみ、左下は次の行が短い場合または最終行のみ。右角は直角 |

Geist の静的 TTF と OFL 1.1 は `assets/fonts/` に同梱する。各サイズを直接ラスタライズし、`scale = 1.25` の 18.75px / 15px も整数へ丸めない。行の座標は小数を保ち、静止時の文字ベースラインだけ整数に合わせる。

行の色は先頭行の中心で A、最終行の中心で B、途中は sRGB チャンネルを線形補間（1 行なら A）。通常→点灯最大の名前色は `mix(c,白,.88)`→`.78`、モード色は `.38`→`.68`。周期点滅・色の回転・縁取り・追加の反射線や装飾は付けない。

描画順は、左 8・下 3 にずらした矩形和集合の影（σ11）→背景ブルーム（σ16）→背景 σ10・彩度1.3 のすりガラスと `rgba(10,11,17,.66)` の地・右からの照り返し→モード発光（σ3.5）・接地影（σ.5）・文字→チューブのグロー（σ4）・本体・白い芯→パルスのグロー（σ6）・芯。すべて専用バッファの真の横→縦ガウスと premultiplied source-over で合成する。半径は `ceil(3σ)`、σ8 以上では半解像度・σ/2 を使用可能。プレートが読む背景は影とブルームを合成した直後のフレームを毎フレーム取得する。

ON は高さ 200ms・右からのスライド 240ms（easeOutCubic）、文字は 60ms 遅れて 160ms。パルスは上端から 220ms（easeOutCubic）で行へ届き、L=1 から 1200ms で減衰。OFF は 220ms の到着後に L=.6 で点灯し、右へ 200ms（easeInCubic）で消え、その開始 80ms 後から高さを 200ms で閉じる。モード変更・並び替えの y と幅、リスト全高は τ60ms で追従する。独立パルスは最大 4 本、古いものから除去する。`pulse = false` でも出入りは続け、パルス・点灯だけを消す。

| 設定 ID | 初期値 | 範囲・効き方 |
|---|---|---|
| `accentA` | `#FF4FA0` | 上端の光色 |
| `accentB` | `#8B6BFF` | 下端の光色 |
| `glow` | 1.0 | 0〜1.6、照り返し・モード発光・ブルーム・チューブグロー・パルスの α |
| `shadow` | 1.0 | 0〜1.6、投影と文字接地影の α |
| `pulse` | ON | パルスと点灯の有無 |
| `scale` | 1.0 | 0.5〜2.0、すべての寸法 |

倍率を掛けた α は 1 にクランプする。`glow = shadow = 0` でもプレートの地・背景ぼかし・文字・チューブ本体・白い芯は残る。昼・夜・白背景、ON/OFF、並び替え、縮尺、空リストの撮影は [gui.md](gui.md) の Edge-lit シナリオを使う。

## 5. ClickGUI の Shadow — 比較と採用

この節の比較・計測・解析メッシュは ClickGUI パネル専用。Edge-lit の影・発光は §4 のフレームバッファと GPU ガウスを使う。

目標: CSS `box-shadow: 0 4px 14px rgba(0,0,0,.38)`（σ = 7 rpx）、図形の外側のみ、sRGB ガンマ空間で合成。

| | 方式 | 評価方法 | 結果 |
|---|---|---|---|
| A | 9-slice テクスチャ | 机上 | DLL はホストのレンダラーにテクスチャを作れない（ImGui 1.91 は `RendererHasTextures` 無し）。フォントアトラスの CustomRect に焼く手はあるが、辺が 2(r+3σ)=52rpx 未満の矩形（折りたたみ 32rpx）は寸法別テクスチャが必要でアトラスが肥大。不採用 |
| B | Direct2D Shadow | 机上 | デバイスを持たない DLL では不可。D3D12 では D3D11On12 の Acquire/Release と毎フレーム Flush が必要。不採用 |
| C | ガウスぼかしシェーダー | 机上 | オフスクリーン RT・2 パス・シェーダーが必要で DLL から不可。不採用 |
| D | 解析的ピクセルシェーダー | 机上（指示書の例外規定） | DLL からホストのパイプラインへシェーダーを差し込めない。不採用 |
| **E** [追加] | **解析的 CPU メッシュ** | **実装・実測** | **採用** |

方式 E: 影 = 図形と 2D ガウスの畳み込みを走査線ごとに分解（直線部は erf の閉形式、角の区間のみ数値積分）。これを CPU でテンソル格子の頂点に評価し、頂点 α の線形補間で描く。格子は輪郭付近だけ密（間隔 σ/3、補間誤差の理論上限 ≈0.33 階調）、角の正方形は 1px 格子で、図形の内側のセルは描かない（半透明の ArrayList 行にも正しい）。テクスチャ・シェーダー・RT 不要なので D3D11 / D3D12 どちらのホスト backend でも同じコードで動く。erf は A&S 7.1.26（誤差 1.5e-7）、入力を ±10 にクランプ、σ ≥ 0.5px。

| 項目 | 方式 E の実測 |
|---|---|
| 断面（白背景・150×600rpx、1920×1080、s=1.0） | 参照画像との最大誤差 **1**、RMSE 0.41（43,428 px）。§5.3 表の期待値との差は全点 ≤0.9 |
| 折りたたみ（150×32rpx） | 最大誤差 **1**、RMSE 0.36 |
| s=1.5（2880×1620 @144dpi、225×900 / 225×48 px） | 最大誤差 **1**、RMSE 0.42 / 0.36 |
| 角丸の一致 | 上記の最大誤差は角を含む全外周での値（ヒートマップ `docs/shadow/error_*.png`） |
| Banding | 参照との差 ≤1 階調。ディザ不要 |
| GPU 時間（RTX 3060 / 32.0.16.2101、open シナリオ 1920×1080、60 フレーム捨て 600 計測、disjoint 除外後 451〜543 サンプル） | 影の区間 中央値 **0.025ms**、p95 **0.037ms**。UI 全体 ON 0.037ms / OFF 0.007ms（差 ≈0.030ms）。目安 0.10ms 以下 |
| GPU メモリ | 専用リソース 0 バイト（ImGui の動的 VB を使用：1 パネル約 3,300 頂点 @s=1、5,400 @s=2） |
| CPU | メッシュは (寸法, 半径, σ, offset) をキーに LRU 96 件キャッシュ。移動・スクロールでは再計算なし。展開アニメーション中は高さが毎フレーム変わるため該当パネルのみ再評価（GPU リソースの再作成は 0 回） |
| Resize / DPI / Device Lost | GPU 状態を持たないため作り直し不要 |
| 実装量 | `Shadow.cpp` 約 280 行、backend 固有コード 0、外部依存 0 |

参考: Evan Wallace "Fast Rounded Rectangle Shadows"（CC0、走査線分解の考え方のみ参照し自作）https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/ 、vello#1422（erf オーバーフロー対策）、dbaron（blur = 2σ）、ImGui #1329、VLC MR 7198、stereopsis shadowrect（内側を描かない最適化）。コードの流用はなし。

参照画像: `medicon_shadow_reference`（`tools/shadow_reference.cpp`）。4× 超標本化マスク → 縮小 → 画素積分ガウス（`std::erfc`）→ オフセット → ×不透明度 → 図形外のみ → 白に合成。GUI の解析モデルとは独立の実装。

## 6. 逸脱ログ

以下は従来設計時の判断記録。外観刷新後の寸法・色は §3 を参照。

| # | 項目 | 理由 | 影響 |
|---|---|---|---|
| D1 | ClickGUI の影は方式 E（CPU 解析メッシュ）を維持。A〜D は旧設計での机上評価 | 既存の精度・負荷要件（±6、0.10ms）を満たすためパネルは変更しない | Edge-lit の専用 GPU ガウスとは別経路 |
| D2 | 折りたたみ時の高さは 32（区切り線 1 を含めない）。展開時の内容高さに枠線 1 を加算 | 指示書の式だと折りたたみ 33・最下行が枠線に 1px 食い込むため（§5.5 は 150×32 を要求） | 折りたたみ 32、展開時は最下行の下に余白 4 + 枠線 1 |
| D3 | Keybind（§4.5.6）は Setting 型ではなく Module の属性なので、Setting ブロック末尾の擬似行として表示（Setting を持つ Module のみ）。Setting の無い Module は中クリック | 既存 Setting の variant を変更しない | Setting の無い Module には Chevron が出ない |
| D4 | ClickGUI のフォント px は整数に丸める（`round(rpx × s)`） | ClickGUI の従来レイアウトを維持 | Edge-lit は 1/64px のキーで小数サイズを直接ラスタライズ |
| D5 | 旧 ArrayList の Segoe UI Regular は廃止 | Edge-lit 指示の Geist SemiBold 600 / Geist Mono Medium 500 を同梱 | ClickGUI Header の Semibold は維持 |
| D6 | 日本語フォールバックは Yu Gothic UI（`YuGothM.ttc` 面1）→ Meiryo UI。全ロールで Regular を使用 | Yu Gothic UI Regular は `YuGothM.ttc` にある。太さ別に TTC を読むと各 14MB 必要 | ヘッダーの日本語も Regular の太さ |
| D7 | ImGui 1.92 未満のため、使用文字だけを集めてアトラスを作り、スケール・文字集合が変わったときに再構築。ホスト所有アトラスでは追記のみ（個別削除不可） | ホストの `ImFont*` を無効化しないため | ホスト所有では旧面が残る。Preview と自動接続の所有アトラスは再構築で破棄。外部データ・範囲の寿命はホストを保護する |
| D8 | 文字列 Setting（既存の型）は読み取り専用表示。旧 GUI の検索欄・Load/Save ボタン・テキスト入力は削除 | 指示書のスコープ外（テキスト入力・検索なし）。保存は自動 | 設定ファイルの手動再読込 UI は無い |
| D9 | `set_module_settings_renderer` の独自 UI は、Module 展開中に別の ImGui ウィンドウとして表示 | ImGui ウィジェットを自前描画のパネル内に埋め込めない | 既存の拡張 API は維持 |
| D10 | ClickGUI / ArrayList は `Category::Client` の Module（設定保持用）。ClickGUI Module の enabled = GUI 表示状態 | 既存 Config に設定を保存するため | 起動時は従来どおり GUI を開く |
| D11 | Preview は D3D11 のみ（`--backend d3d12` はエラー） | Preview の描画ホスト仕様 | 自動接続の D3D12 は D3D11On12 経由で Edge-lit を実行 |
| D12 | ゲームの HUD 非表示（F1）連動は未対応 | 既存コードで状態を取得できない | ユーザー実機確認項目 |
| D13 | Edge-lit の GPU 資源は専用レンダラーが保持 | Present / Present1 の自動接続と Preview で実行 | リサイズ・デバイス再作成で解放し、ホスト状態を復元。外部ホストは専用 C API を呼ぶ |
