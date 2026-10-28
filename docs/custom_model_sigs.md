# CustomModel: sig・SDK確認記録

最新更新（2026-10-28 JST）: P0統合版の動作をユーザー確認後、P1を実装して直方体の実機表示を確認。
追加sig・ABI・native所有権・検証結果は[P1報告](custom_model_p1_report.md)と[P1 sig JSON](../data/signatures/custom_model_p1.json)。
以下の未登録/実機未確認という記述は取得時点の履歴。P2には進んでいない。

調査日: 2026-10-28（日本時間）。**近距離/ECS側2sigで3人称が消えるとユーザー報告あり。追加指示でDataDrivenRendererを腕の抑止用に併用しReleaseビルド済み。統合版の実機効果は未確認。各sigは元EXEで1件一致。**
静的なパターン一致、文字列参照、上流の型宣言は、実行可能なABIや描画範囲の保証とは区別する。

最新状態は[queueRenderEntities再調査](custom_model_p0_queue_candidates.md)と[P0実装報告](custom_model_p0_report.md)。
DataDrivenRendererは単独では手だけ消えたが、今回の追加指示で近距離/ECS側と組み合わせて再利用する。
全視点でplayerが残ったaddPlayers候補は登録対象から外したまま。
以下の段階別記録で「未試験」「取得時点で停止」とする箇所は、その時点の履歴である。

## 後続更新: IDA MCPで取得した現行入口

`DataDrivenRenderer::render(BaseActorRenderContext&, ActorRenderData&)` をVA `0x14558D300` / RVA `0x558D300` に特定した。
戻り値はvoid、RCX=this、RDX=context、R8=renderData。ソース位置情報の完全な関数名文字列と実際のレジスタ使用を照合した。
この入口は、以下の旧7引数ActorRenderDispatcherとは別物。

- [IDA調査報告](custom_model_ida_report.md): ABI、読取位置、P1候補、未確認点、実機との区分。
- [9本のsigと検証情報](../data/signatures/custom_model_candidates.json): MCP生成後、元PEで各1件だけの一致を再確認。ランタイムのカタログへは未登録。
- [P0用SDK宣言](../include/medicon/sdk/CustomModelRender.hpp): opaque型と根拠のあるABI/位置のみ。

以下の上流調査は初回時点の履歴として残す。「不足」のうち入口sig/ABIは後続のIDA調査で解決し、P1はAPI候補の回収まで進んだ。

## 対象と再現方法

- 上流: [surukotonai/solstice](https://github.com/surukotonai/solstice/tree/1d7808f52ef4ba6e34a7fbcc65294907a7761611)、commit `1d7808f52ef4ba6e34a7fbcc65294907a7761611`。リモートHEADとローカルcheckoutが一致し、`SigManager.hpp` にローカル変更なし。
- 実行ファイル: `C:\Users\hina\AppData\Roaming\levilauncher.exe\versions\1.26.52.03\Minecraft.Windows.exe`。
- FileVersion: `1.26.52.3`、SHA-256: `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`。
- PE TimeDateStamp: `0x6AB54E37`、SizeOfImage: `0x12C02000`。このディスク上の実行ファイルのみを照合し、実行中のモジュールは未検証。
- 全体のIDA解析は行わず、下記5パターン・指定文字列・その近傍命令のみを確認した。

```powershell
python .\tools\inspect-custom-model-prerequisites.py `
  --exe 'C:\Users\hina\AppData\Roaming\levilauncher.exe\versions\1.26.52.03\Minecraft.Windows.exe'
```

結果は `out/custom-model/prerequisites.json`。完全なsig、上流行番号、実行ファイルのハッシュ、一致RVA、文字列参照候補を含む。
スクリプトはパターンを上流から読むだけで、クライアントのsigカタログへ登録しない。

## 初回調査: 旧ActorRenderDispatcherは不採用

上流 [`src/SDK/SigManager.hpp:49`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/SDK/SigManager.hpp#L49) の
`ActorRenderDispatcher_render` は **1.21.51用・コメントアウト済み**。対象PEの実行可能セクションに **一致0件**。

```text
48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?
48 81 EC ? ? ? ? 0F 29 B4 24 ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4
48 89 45 F7 4C 89 4C 24 ?
```

上流の候補宣言（**現行版では未検証・呼び出し禁止**）:

```cpp
void render(ActorRenderDispatcher* self, BaseActorRenderContext* context,
            Actor* actor, glm::vec3* cameraTargetPos, glm::vec3* pos,
            glm::vec2* rot, bool ignoreLighting);
```

根拠:

- [`ActorRenderDispatcherHook.hpp:22`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/Hook/Hooks/RenderHooks/ActorRenderDispatcherHook.hpp#L22): 上記宣言。
- [`ActorRenderDispatcherHook.cpp:32`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/Hook/Hooks/RenderHooks/ActorRenderDispatcherHook.cpp#L32): Hookの作成自体がコメントアウト。
- [`src/Hook/HookManager.cpp:60`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/Hook/HookManager.cpp#L60): 登録も `//ADD_HOOK(ActorRenderDispatcherHook); //die`。

`BaseActorRenderContext.hpp` は `mScreenContext`・`mFrameAlpha` 等を宣言するが、1.26.52.3での配置・寿命は未検証。
そのオフセットをクライアントへ転記していない。

## 既存sigの静的一致（採用前の候補）

全パターンは既存の `data/signatures/solstice.json` にある。同名エントリと監査JSONを参照。
以下はパターンの一意性のみ確認し、関数の完全なABI・実行時の引数・描画対象は未検証。

| 上流名 | SigManager.hppの行 | 一致数 / RVA | 用途の限界 |
| --- | --- | --- | --- |
| `ItemRenderer_render` | 92 | 1 / `0x55AAA60` | 上流は `void(ItemRenderer*, BaseActorRenderContext*, ActorRenderData*)`。ItemActorにも使われる型で、プレイヤーの左右の手を描く入口とは確定できない。P4へ流用しない。 |
| `ActorShaderManager_setupShaderParameters` | 106 | 1 / `0x1EB5E30` | シェーダ引数設定候補。プレイヤー全描画を止める入口やmaterial取得APIではない。 |
| `LevelRenderer_renderLevel` | 152 | 1 / `0x47F8570` | レベル描画候補。ローカルプレイヤーのみを止める入口として使えない。 |
| `Unknown_renderNametag` | 122 | 1 / `0x1D55770` | P5の調査候補。高さの引数・配置・取得経路は未検証。 |

## 文字列から絞った調査箇所

RVAは上記ハッシュのPEに対する**調査用の値**。ランタイム用定数にしていない。
監査スクリプトの `possible_lea_xrefs` はバイト列候補であり、命令境界まで保証するものではない。
次の3参照は `llvm-objdump` の近傍表示でLEA命令を追加照合した。

| 手がかり | 静的な結果 | 未確認事項 |
| --- | --- | --- |
| `controller.render.player` | 部分文字列3件。`0x4482A28` → `0xEC1AD36` と `0x4482A6B` → `0xEBFC8EE` にLEA。両方ともPE unwind範囲の開始は `0x4482940`。 | 近傍は文字列構築と定数設定。プレイヤー描画関数とは認定できない。登録先・呼出元を限定して追う必要がある。 |
| `rightItem` | ASCII完全一致の部分文字列0件。 | 別表記や外部アセットの可能性。関数が存在しないという意味ではない。 |
| `textures/models/armor/` | 部分文字列12件。監査のRIP相対LEA検索では直接参照0件。 | 間接参照・文字列途中への参照などは網羅していない。 |
| `entity_alphatest` | 部分文字列9件。複数の参照候補あり。 | 文字列だけではmaterial取得API・所有権・描画順序を証明できない。 |
| `Tessellator` | 部分文字列5件。`0x192CD78` → `0xEBFCCE9` のLEAを照合。unwind範囲開始は `0x192BFC0`。 | `begin/vertex/end` やオブジェクト取得のABIは未特定。 |
| `renderMeshImmediately` | ASCII部分文字列0件。 | 最適化や別名を除外できず、相当APIの不存在とは判断しない。 |

## 必要な追加資料

| 段階 | 必要な情報 | 取得状況 |
| --- | --- | --- |
| P0 | 現行のプレイヤー/アクター描画入口sig、一意なRVA、戻り値・全引数・呼出規約・this調整、callsiteと型の根拠 | **後続IDA調査でDataDrivenRenderer入口のsig/ABIを取得済み**。renderPosと体の向きが補間済みである根拠、1人称/プレビューの区別は未確認。 |
| P0 | 同じ入口のorig省略で体・防具・手持ち・マントを抑止できるか | **実機未検証**。別パスなら各関数のsigとABIが必要。 |
| P1 | Tessellator取得・begin/vertex/color/uv/normal/endの現行sigとABI、各型とバッファ寿命 | **後続IDA調査で取得経路とbegin/vertex/end候補sigを回収**。属性のインライン読書きも確認。完全な型とバッファ寿命は未確定。 |
| P1 | `MeshHelpers::renderMeshImmediately`相当のsig・ABI、ScreenContextの取得と行列スタックのpush/pop | **後続IDA調査で相当候補とScreenContext経路を回収**。native型・描画条件、公開push/pop APIは未確定。 |
| P1 | entity系materialの取得、既存textureの取得・ハンドルABI、深度と描画フェーズの根拠 | **後続IDA調査でentity_alphatest getter等の候補を回収**。owner取得、native texture型/寿命、深度は未確定。 |
| P2以降 | Raw ResourceLocationのtexture登録、手持ち描画API、ネームタグ高さ等 | 今回の実装対象外。P1報告の確認後に個別検証する。 |

追加資料には対象バイナリの版・ハッシュ、sigと一致数、RVA、アドレス付きcallsite/逆アセンブル、静的確認/動的確認/未確認の区分を付ける。
IDAの全体再解析は不要。上記の文字列参照・関数候補と直近の呼出元に対象を絞る。

## P0診断実装への接続（2026-10-28追記）

### 追加取得: `ActorRenderCandidates::addPlayers`のcallback候補

ユーザーの再調査依頼により、RVA `0x480E6C0`の111-byte sigを取得し、元PEで一意一致を確認した。
プレイヤーの描画候補を集めるcallbackの静的候補で、3人称のローカルplayerに適用されるか・完全なABI・実行時の効果は未確認。
詳細とsigは[候補報告](custom_model_p0_addplayers_candidate.md)および[候補JSON](../data/signatures/custom_model_p0_addplayers_candidate.json)。
ユーザーの指示により、Hook実装や実機試験に進まずこの取得時点で一時停止。

**後続の実機結果: 現候補NG。** 1人称の手が消え、3人称プレイヤー本体は残るとのユーザー報告を受けた。
sigの一致とDataDrivenRendererとしての識別根拠は維持するが、P0の目的を満たす入口とは認定しない。
`CustomModelRender.hpp`の`live_probe_candidate_eligible = false`により、次ビルドでは現候補のHookを登録しない。
v2/DataDrivenRendererRuntimeの3参照まで限定追跡したが、代替入口/ABIはまだ未確定。

`src/sdk/CustomModelProbe.cpp`から既存SignatureRegistryを呼び、DataDrivenRenderer入口の1本だけを解決する。
元PEのFileVersion、timestamp `0x6AB54E37`、SizeOfImage `0x12C02000`も確認する。
`src/platform/AutoOverlay.cpp`で既存HookManagerのcreate/enable_allを使用し、既存native初期化の後に追加した。
停止時は同じ管理機構でdisableし、既存callback drainを待つ。
ローカル判定は既存BedrockChat::is_local_player。OFF/不正読取/非対象は同じ3引数でorigへ通す。
旧7引数ABI、P1候補sig、未確定のcontext/ScreenContext配置は実行に使用しない。
F8・設定・呼出数/Present・actor/context・ON/OFF FPS・probe CPUの診断実装は完成したが、ゲーム上の描画範囲は未検証。
詳細は[P0実装報告](custom_model_p0_report.md)。上流sigカタログと既存Hookの相対初期化順序は変更していない。

## P2 静止FBX（2026-10-28）

ユーザーのP2着手指示により、TextureGroupのasync RTTI参照とP1 callerを範囲限定で調査。
texture get `141D0CCE0`、TessEnd `1459CBD50`、built Mesh draw `144B05040`、Mesh destructor `144AFFD10`、builder clear `144B05570`、current view ID `1446EEA60`を取得。
6本とも元PEで各1件。sig bytes・版・SHAは[data/signatures/custom_model_p2.json](../data/signatures/custom_model_p2.json)、物理ABIとcallsite・layout・所有権根拠・未検証項目は[P2報告](custom_model_p2_report.md)。
P2試験DLLの再読込後、ユーザーがモデル表示と「見た目は正常・カクつきなし」を確認した。P3以降の関数は追加しない。

## P3 アニメーション入力（2026-10-28）

ユーザーのP3指示により、native swingのread-only profileを2本追加した。
`1447722D0`はtickとdurationからfloat swing値をActor+40Cに格納し、`142077CC0`のprefixはそれを前回値+450へコピーする。
各64-byte sigは対象PEで一意。これらの書き込み関数は実行・hookせず、読み取りlayoutのversion gateにだけ使う。
bytesは[custom_model_p3.json](../data/signatures/custom_model_p3.json)、component読取・一括Tessellator転送のnative根拠と計測範囲は[P3報告](custom_model_p3_report.md)。
描画は既存P1/P2の経路で、P3によるnative hook追加はない。実機の動作/FPSは確認待ち。

## P3 向き・一人称の修正（2026-10-28、未ビルド）

初版P3に対する向きの逆転と一人称遮蔽の報告を受け、視点設定の読み取りを追加した。
対象FileVersion `1.26.52.3`、SHA-256 `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`をIDA入力と元PEで照合。
背景全体解析を停止した状態で、以下の関数と近傍・vtableだけを調べた。調査終了時も背景解析は停止。

| 追加sig | VA / RVA | 長さ・元PE内一致 | 根拠・用途 |
| --- | --- | --- | --- |
| view_options | `145DC28D0` / `5DC28D0` | 8 bytes、1件 | ClientInstance vtable `14E9731B0`の174番。`mov rax,[rcx+E10]; ret`。Optionsを保持するフィールドを読み取る根拠 |
| view_option_ref | `140FB4D50` / `FB4D50` | 28 bytes、1件 | Options vtable `14E7FC490`の1番。物理引数(this, pointer出力, option ID)で140FB4B10を呼び、借用pointerのみ返す |
| view_option_array | `140FB4B10` / `FB4B10` | 46 bytes、1件 | 有効IDで`[options+10+id*8]`を読む。無効時のignore option構築は呼び出さない |

全bytesは[custom_model_p3.json](../data/signatures/custom_model_p3.json)と`CustomModelP3Patterns.hpp`。実行時は既存P0/P1の版・PE gate後、既存SignatureRegistryで3本すべての一意解決を要求する。RVAを実行時アドレスとして直接採用しない。
3関数ともhook・呼出しを行わず、毎callbackでnative vtable entryが解決済みアドレスと一致するか確認する。

SDK照合元は`.reference/solstice/src/SDK/Minecraft/ClientInstance.cpp`のgetOptions、`OffsetProvider.hpp`の174番、`Minecraft/Options.hpp`のIntOption。
現行版では`1420B98D0`がOptionsの2本のvtableと配列を初期化し、`1420BC0C0..17C`が`game_thirdperson` / `options.thirdperson`をID=3として構築する。
そのため取得先はOptions+`10+3*8=28`。`140921520`はIntOptionの+10 maximum、+14 minimum、+18 current、+1C defaultを設定し、派生enum optionの`1409221C0`はImpl+1D4を6に設定する。
`145DBB130..21E`の実際の設定読取では、Option+8のImpl、その+238の上書きOptionをたどり、最終Option+18を読む。新readerもこの上書きを最大8段まで追い、kind=5/6・min=0・max=2・current=0..2を検証する。native objectはコピー読取だけで、所有権操作・保持・allocationを行わない。

視点は0=一人称、1=三人称後ろ、2=三人称前。`CameraComponent.hpp`のモード一覧とも照合。全身カスタム描画は後ろ・前の三人称に限定し、設定を読めない場合はunknownとして描画を控える。
上記は静的解析と元PE上のsig照合結果。**ユーザーの指示により修正後のビルド・実機検証は未実施。**
監査データは`out/custom-model/ida/p3b_*_evidence.json`、`p3b_option_constructor.asm.txt`、`p3b_view_patterns.json`。

後続の指定「一人称はCustommodelの手」に従い、full-bodyを除外したまま、workerが腕の三角形だけを抽出し、既存P3のnative描画入口で描く方式に変更。
カメラ相対配置は自前のview-model行列で、native SDK offsetの追加はない。カスタム腕が実際に描けた後だけバニラの腕を抑止し、準備不足時は元の描画を通す。
この変更とCustomModel/Neonの表示名変更も未ビルド。詳細は[P3報告](custom_model_p3_report.md)。
