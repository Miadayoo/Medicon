# CustomModel P1 実装・実機確認報告

2026-10-28 JST。P1のnative直方体描画を実装し、ゲーム画面で表示を確認した。
ユーザーから移動・回転への追従、壁による遮蔽、ガラス越し、水中の表示がすべて問題ないとの報告を受けた。
P1の表示・追従・遮蔽確認後に停止して報告済み。続くユーザーの「p2進んで」によりP2へ着手した。
P1のFPS同一条件比較と、法線による陰影だけの判別は未完了のまま。[P2報告](custom_model_p2_report.md)に後続作業を記録する。

## 成果物と操作

- 実機確認したDLL: `D:\code\Medicon\out\build\local\bin\Medicon-P1b.dll`。
- SHA-256: `21acfce2fbcc91ea88812ede1f9d3a30ab4a2a98f856350fc455bea4e13db291`。
- 同じソースの通常Release出力: `D:\code\Medicon\out\build\local\bin\Medicon.dll`。
- F8はCustomModelProbe全体のON/OFF。初期OFFを維持。
- `P1 Test Box`は直方体のON/OFF。OFFならP0の抑止だけを試せる。
- `Material`は`entity_alphatest` / `entity`、`Brightness`は0.05〜1.0の頂点カラー乗算。
- `Show Debug`は独立した設定。F8で診断窓を自動表示しない。

## 実装したファイル

| ファイル | 内容 |
| --- | --- |
| `include/medicon/sdk/CustomModelBox.hpp` | 観察したnative ABI、値だけのpose/行列、描画scope、診断カウンタ |
| `src/sdk/CustomModelBox.cpp` | 36頂点の1×2×1直方体、6面の色・UV・法線、native材質・テクスチャ参照、状態復元 |
| `include/medicon/sdk/CustomModelP1Patterns.hpp` | 対象PEで一意性を確認したP1パターン |
| `data/signatures/custom_model_p1.json` | 対象バージョン、sig、RVA、一致数、実機結果と根拠 |
| `include/medicon/sdk/BedrockChat.hpp` | 既存の検証済みlocal-player getterを描画callback内で使うための最小入口 |
| `src/platform/AutoOverlay.cpp` | 既存HookManager経由のP1登録、native callbackの生存期間、Show Debug表示 |
| `include/medicon/modules/CustomModelProbeModule.hpp` / `src/modules/CustomModelProbeModule.cpp` | Test Box / Material / Brightness設定 |
| `tests/custom_model_box_tests.cpp` / `CMakeLists.txt` | 形状・ABI・所有権・状態復元・scope・OFFの検証とビルド登録 |

## 完了条件ごとの結果

| 条件 | 結果と根拠 |
| --- | --- |
| プレイヤー位置に直方体、移動・体の回転に追従 | **OK（ユーザー実機確認）**。位置・騎乗offset・body yawはゲームの補間関数を使用。自前のtick補間はしない |
| 壁の後ろで隠れる | **OK（ユーザー実機確認）**。ゲームのMesh送信経路とentity materialを使用 |
| ガラス越し・水中 | **OK（ユーザー実機確認）**。透明描画の全状況・全materialの組み合わせを網羅した試験ではない |
| 明るさを変更できる | **OK**。UIで1.0→0.25へ変更し、直方体が暗くなることを画面で確認。昼〜夜の描画も継続。洞窟そのものの独立試験は未記録 |
| 既存テクスチャ付きの描画 | **OK（表示確認）**。ゲームが送信したnative bindingをnative copyで参照保持。画像名は未回復。採用された画像は細かい模様で、モデル用の仕上がりではない |
| 法線の入力と陰影の報告 | **入力OK**。各面の4-float normalと属性有効flagをTessellatorへ渡す。法線だけに由来する陰影は、面ごとの色・テクスチャ・環境変化と分離できていない |
| OFF復元 | **表示OK**。F8 OFFで直方体が消え、Alexのバニラ表示へ戻ることを画面で確認 |

## material比較

| material | 実際に見えた差 |
| --- | --- |
| `entity_alphatest` | テクスチャの透明部分で面が抜ける。採用画像が細かいため、点状の模様・抜けが多い |
| `entity` | 同じ画像を使用しても黒い背景を含む不透明な面が見える。alphatestより直方体の輪郭が明確 |

どちらもmaterial lookupはゲーム自身に行わせる。DLLの`std::shared_ptr`や`std::string`をnative ABIへ渡さない。
画像の選択は最初の適格な既存bindingで固定し、その後の描画callbackから生ポインタだけを持ち出さない。
保持するのはnative copyが参照カウントを増やしたテクスチャresourceだけ。actorとrender contextはcallback内限定。
metadataはnative switch tableでコピー/破棄に所有物がないと確認したtag 0の値だけを採用する。

比較画像:

- [entity / Brightness 1.0](../out/custom-model/p1/entity-brightness-1.png)
- [entity / Brightness 0.25の設定と表示](../out/custom-model/p1/entity-brightness-025-settings.png)

## 確認したnative関数と物理ABI

すべてMinecraft `1.26.52.3`、ImageBase `0x140000000`。
元EXE SHA-256は`47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`。
PE timestamp `0x6AB54E37`、SizeOfImage `0x12C02000`。
完全なsigは[JSON](../data/signatures/custom_model_p1.json)と[コンパイル用ヘッダー](../include/medicon/sdk/CustomModelP1Patterns.hpp)。各パターンは元PEの実行可能セクションで1件一致。
実行時はP0のファイルバージョンgateの後、既存SignatureRegistryで一意解決する。
これは実行中image全体の暗号学的ハッシュ照合を行ったという意味ではない。

| RVA | 物理呼び出し境界 / 根拠 |
| --- | --- |
| `0x4686F20` | `void(camera, BaseActorRenderContext*, ViewRenderObject*, client)`。ソース位置情報の完全な`LevelRendererCamera::render`名とRCX/RDX/R8/R9保存を照合 |
| `0x46B95F0` | `void(screen, tess, material, textureBinding, metadata)`。`RenderMeshImmediately`文字列、native Meshの生成・送信・破棄まで追跡 |
| `0x46EEE30` | `void(screen, tess, material, textureBinding, uint16 viewId, metadata)`。既存actor/item callerの6個の物理slotと本体を照合 |
| `0x4B04540` | `void(mesh, renderContext, material, bindings, unused5, unused6, metadata, unused8, optional)`。本体は引数1〜4/7/9を使用。5/6/8は観察した未使用slotとしてそのまま転送 |
| `0x59CA2E0` | `void(tess, unusedRdx, uint8 mode, int count, uint8 indexed)`。本体は入力RDXを使わない。mode 2の三角形36頂点を送る |
| `0x59CA7A0` | `void(tess, float x, float y, float z)`。XMM1/XMM2/XMM3の使用とnormal/UV/colorの読み取りを照合 |
| `0x19F7980` | `Vec3*(actor, Vec3* out, float alpha)`。native dispatcherの呼出し、騎乗時の分岐、出力への書込みを確認 |
| `0x7CB5AD0` | `Vec3*(Vec3* out, actor, float alpha)`。PassengerRenderingRidingOffsetComponent `0x33675BF2`。成功/不存在両方のreturnまで限定decode |
| `0x20874B0` | `float(actor, float alpha)`。実行中local-player vtable +0x68から特定。MobBodyRotation `0xD7F64BBA`の角度補間のみ。描画時もそのvtable targetとの一致を確認 |
| `0x253CC80` | `float(actorEntityContext)`。native DDRがactor+8で呼び、renderPos.yから引くOffsetsComponent `0xDE21C316`の値 |
| `0x52F1E00` | native 16-byte material output / group / HashedString key。ゲームに生成させ、下記releaseで一度だけ破棄 |
| `0x7E380` | native material handle解放。+8のcontrol blockとnative destructorを使用 |
| `0x4B17B20` | `void(tag+1, destinationPointerWrapper, sourceBinding)`。40-byte bindingのコピー、native shared resource参照増加を確認 |
| `0x963F80` | `void(nativeBinding)`。tag別のnative解放。本体・switch tableとnative actor callerの破棄を照合 |
| `0x56020FC` | 関数ではなくRIP anchor。先頭LEAでmaterial group、次のLEAで`entity_alphatest`のnative keyを取得 |

material groupの元VAは`0x151E3B488`、alpha keyは`0x151E299D8`。
実行中の初期化済みkeyをread-onlyで確認した結果、`entity` keyはalpha+0x90（元VA `0x151E29A68`）。
描画前に両keyのhash・文字列・長さを確認し、異なればlookupも描画もしない。
body yawの根拠はsolstice `SDK/EnttHashs.hpp`のcomponent名と、対象バージョンのnative getterを併せたもの。
旧SDKの完全なactor/ScreenContext/STL layoutを流用していない。

## 描画scopeと状態復元

最初の実験ではworld renderのscope内に既存Tessellator送信がなく、drawn=0だった。
下位MeshSubmitも今回のworld scope内では0件で、そこへ接続するだけでは描画できなかった。
最終版は既存送信からnativeテクスチャ参照を取得した後、world render callbackが戻る前に直接native送信する。
実機ではworld=1 / matched=1 / drawn=1を継続して確認した。

- `context+0x28`のScreenContext、`screen+0x18`の行列owner、`screen+0xB8`のTessellatorはnative callerで確認。
- 補間alphaはcontext+0xB0、カメラ原点は`*(context+0xA8)+0x28`。native dispatcherと同じ読み取り。
- world matrixのdeque topはowner+0x48/50/58/60のmap/count/offset/sizeから求める。
- callback入口でbase matrixを値としてコピーし、native補間位置とbody yawのT/Rを掛ける。
- 描画前のTessellator scalar属性・translation/scale/transform flagとmatrixを保存し、描画後に復元。
- native vector、Mesh、resourceの内部所有権をDLLで偽装/破棄しない。native wrapperに生成・送信・破棄させる。
- model matrixを戻した後はowner+0x78をdirtyにする。既に計算された派生行列を再利用させないためで、dirty=falseへの単純復元は行わない。
- busy Tessellator、別context、未準備material/texture、読取失敗ではその回のP1を省略。P0の既存動作は維持。
- stopではP1 disable→P0 disable→既存callback drainの後、保持resourceをnative releaseする。

根拠は`out/custom-model/ida/p1_*.asm.txt`、`original_59ca2e0/59ca7a0/4b05040/5601a40/558d300.asm.txt`、
`p1_live_material_keys.json`、`p1_live_actor_audit.json`、`p1_variant_tables.json`。
IDAの全体autoanalysisを有効にせず、対象SHAとautoanalysis OFFを毎回確認して限定decodeした。
ゲームの観察用PythonはPROCESS_QUERY_INFORMATION | PROCESS_VM_READのみで、remote call/write/injectは行わない。

## テストと性能

- 新P1b DLL自身のexport lifecycleテストが成功。
- `custom_model_box / custom_model_probe / runtime / signatures / bedrock_chat / core / commands / gui`の8テストが成功。
- P1テストは形状寸法、外向き三角形、6色、UV範囲、物理引数、補間alpha、状態復元、native resourceのcopy/release、scope終了後/OFF/未準備時に触らないことを検証。
- 合成native関数によるテスト成功は、ゲームのshaderや深度の合格を代替しない。表示・遮蔽・透明物は上記の別の実機結果。
- 画面の`P1 draw CPU / Present`は観察時に約0.028〜0.060 ms。これは36頂点構築・native送信の区間で、pose取得とmaterial lookupは含まない。
- FPS診断の観察例はOFF平均49.9（6189 frames）、ON平均60.4（16496 frames）。別時刻・昼夜/負荷の異なる累積なので、この差を性能改善や回帰の根拠にはしない。
- 同じ場所・同じ視点で取り直すFPS比較は、Computer Useが物理Escで停止されたため未完了。
- 1万頂点モデルでの0.3 ms目標、描画全体の追加CPU時間、native内部heap確保ゼロは未検証。DLL側の直方体頂点は固定配列だが、native Tessellator/Meshは初回容量確保等を行い得る。

## 未解決と次段階

P1の直方体を表示できない問題は解消した。D3D12独自描画へ切り替える必要性は現時点では示されていない。
残る検証は同一条件のFPS比較、陰影の切り分け、透明描画条件の追加網羅。
画像名を指定した安定したテクスチャ取得はP2のnative texture lookup調査と分けて扱う。
**ここで停止する。FBX取り込み・スキニング・手持ちアイテムのP2以降の実装は行っていない。**
