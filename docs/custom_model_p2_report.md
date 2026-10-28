# CustomModel P2 静止FBX

2026-10-28 JST。「p2進んで」に基づく実装。P3以降は対象外。
**P2完了。試験DLL再読込後、ユーザーがモデル表示と「見た目は正常・カクつきなし」を確認した。P3には進んでいない。**

## 操作と資産

- CustomModelProbeの`Mode = Custom model`でFBX。F8でON/OFF、起動時OFF。Show Debugは独立設定。
- `Test box`はP1の箱、`Hide vanilla`はP0の確認。BrightnessはP1の頂点色のみ。
- DLL横の`models/neon-t-pose/model.fbx`を優先し、なければ既存クライアントデータフォルダの同じ相対配置。
- `model.json`の`scale`既定1.0、`yawOffset`既定180.0。再起動時反映。
- Downloadsの原本は変更せず、検証用DLL横へハッシュ照合してコピー。
- FBX SHA: `8d036bc5e8ec3536e49a648113af2b281f9d5710b45deb3255cdd2f2994b2210`。
- 髪・体・目の3材質、19,952三角形、59,856頂点、native Mesh 17分割。PNGは髪1024²、体2048²、目512×256。
- メートル/Y-up boundsは`(-1.051655,-0.009463,-0.199576)`～`(1.050766,2.119762,0.319178)`。

## 変更ファイル

| ファイル | 内容 |
| --- | --- |
| `third_party/ufbx/` | upstream v0.20.1、commit `13b5df8e89eff0ce898f7c37fa0716e3bc186573`、MIT選択 |
| `include/medicon/custom_model/ModelAsset.hpp`, `src/custom_model/ModelAsset.cpp` | worker import/PNG検査/cache/immutable公開 |
| `include/medicon/sdk/CustomModelStatic.hpp`, `src/sdk/CustomModelStatic.cpp` | 物理ABI、Raw入力、既存SignatureRegistryによる解決 |
| `include/medicon/sdk/CustomModelP2Patterns.hpp`, `data/signatures/custom_model_p2.json` | 6関数の一意sig、対象PE |
| `src/sdk/CustomModelBox.cpp` | P1 scope共有、分割native Mesh構築・再利用・破棄、行列/属性復元 |
| `src/platform/AutoOverlay.cpp` | P1の後に既存HookManagerでtexture hookを追加、worker、完成待ちfallback、診断 |
| `src/modules/CustomModelProbeModule.cpp` | Mode設定、Show Debug維持 |
| `tools/custom-model-inspect.cpp` | ゲームなしの実資産解析CLI |
| `tests/custom_model_fixture.hpp`, `tests/custom_model_asset_tests.cpp`, `tests/custom_model_box_tests.cpp` | 自作FBX/PNG、cache/async/native契約・所有権の検証 |
| `CMakeLists.txt` | ufbx C99、assetライブラリ、DLL/SDK/CLI/test接続 |

## workerと描画

ufbxは`target_unit_meters=1.0`、`target_axes=ufbx_axes_right_handed_y_up`、`evaluate_skinning=true`。
nodeのgeometry変換と法線逆転置を適用し、PNGのVを反転。P3 animatorは未実装。
公式: [load options](https://ufbx.github.io/reference)、[node transforms](https://ufbx.github.io/elements/nodes/)。

ファイル読込・FBXパース・三角形化・hash・PNG検査・cacheはjthread上のみ。
埋め込みPNGはufbx contentメモリから取得し、workerがhash名PNGに展開してnative Raw絶対パスで登録。
埋め込みJPEG、layered/procedural diffuse、UVなしは失敗としてバニラ維持。
外部PNGはFBX相対パスまたはtextures内の同名ファイル。モデルフォルダ外を開かない。
`.mmdl`はimporter/version、FBX/設定/PNGのSHA、bounds、partsと頂点binary/hashを保持。変更・破損・サイズ異常はsourceから再構築。

完成した`shared_ptr<const ModelAsset>`をatomic公開。worker所有者にも強参照を残し、render threadで大きな資産を破棄しない。
GPU準備はworld callbackあたり4095頂点の1chunk。完成済みnative Meshを毎frame再利用。
texture hook callbackの借用TextureGroupから元trampolineの非同期分岐でPNGを要求。group/actor/contextは持ち越さない。
全chunk・全PNG・native empty metadata・materialが揃うまで、P0抑止とFBX描画を開始しない。
停止はhook無効化→callback drain→native Mesh/texture解放→worker join。OFFで抑止・描画停止。
サーバーパケット、skin、actor/world状態は変更しない。変更したTessellator属性・world行列は復元。

## native根拠

Minecraft.Windows.exe 1.26.52.3、timestamp `6AB54E37`、SizeOfImage `12C02000`、ImageBase `140000000`。
SHA `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`。
IDA autoanalysisはOFFを維持。P1入口とTextureGroup async lambda RTTIから範囲限定で追跡。
6関数の64-byte sigは元PE実行可能section全体で各1件。bytesはJSON。

| VA (RVA) | 物理ABI・根拠 |
| --- | --- |
| `141D0CCE0` (`1D0CCE0`) | `(group,out32,ResourceLocation*,bool,uint64,uint8)`。caller `1407EFF07`。`141D0D57F`でbool非0がasync分岐`D797`へ |
| `1459CBD50` (`59CBD50`) | `(tess,outMesh,int,string_view16*,uint16)`。P1 caller `1446EEE8E..EEEB3`、mode2とnative view ID |
| `144B05040` (`4B05040`) | `(Mesh*,ScreenContext+10,material16*,binding40*,unused,unused,metadata40*)`。input Meshを破壊しない |
| `144AFFD10` (`4AFFD10`) | `(Mesh*)`、bounded end `144AFFE91`。P1 callerのnative destructor。外側storageをfreeしない |
| `144B05570` (`4B05570`) | `(builder=tess+8)`、leaf end `144B0569E`。native vectorsのend/boundsをclear |
| `1446EEA60` (`46EEA60`) | 引数不使用、native view ID返却。全bodyを`1446EEBC1`まで確認 |

ResourceLocationは56 bytes: kind DWORD+0、native string+8（len+18/cap+20）、path hash+28、combined hash+30。
kind2は`.reference/solstice/src/SDK/Minecraft/mce.hpp`のexternal constructorが根拠。古い40-byte全layoutは流用しない。
現行constructor `1402709A0..270BE1`でhash確認: FNV-1a basis `CBF29CE484222325`、prime `100000001B3`。
combinedは`((basis XOR kindByte)*prime) XOR pathHash`。
borrowed入力はread-only。getTexture内の`144B0F000`がゲームallocatorでコピー。DLL STLをゲームに渡したり相互allocatorで破棄しない。
output32はnative参照pair×2、tag1を追加したbinding40で保持する。
出力copyは`141D0D6E2..D777`、tag1 copyは`144B17B80..BBC7`、破棄は`140963FBC..64034`。
GPU readyはcaller `1407EFF29..F64`と同じTextureData+8→GPU+12 bit0。試験DLLによるモデル表示はユーザー確認済み。
Meshはnative出力632 bytesを1024-byte aligned opaque storageに構築し、コピーせずnative destructorで解放する。

## 検証結果と残る測定

実FBXの新規importと次回cache hitを確認。三角形・chunks・PNG数は一致。
Releaseビルド成功。2026-10-28の関連10テスト（custom_model_asset/box/probe、runtime、dll_lifecycle、signatures、bedrock_chat、core、commands、gui）は全件成功、8.47秒。
埋め込みPNGをcontentメモリから展開するfixtureと、worker成功時のatomic公開も成功。
試験DLL: `D:\code\Medicon\out\build\local\bin\Medicon-P2.dll`（通常Medicon.dllと同一bytes）。
SHA-256: `826bffe4f135605607a1f65eb94797e0cf23dd49f0984ef3ce7758669a490ba4`。
自作1三角形FBXでUV/単位/cache roundtrip/FBX・設定hash変更/破損cache/不正PNG/workerの未完成未公開を検証。
native mockはasync物理slot、PNG待機fallback、一度構築→複数frame再利用、状態復元、OFF、参照解放を検証。
これは呼出契約のテストで、ゲーム表示成功は示さない。

試験DLLを再読込したユーザーから「読み込んだ・モデルが出た」との報告を受けた。
続いて、三人称での向き・大きさ、顔／体／髪のテクスチャと、F8 ONから表示までの目立つカクつきを質問し、「見た目は正常・カクつきなし」と回答を受けた。
P2の完了条件（基本姿勢の表示、向き・大きさ・UV、読み込み時の目立つフレーム落ちなし）は、このユーザーの実機確認に基づき合格とする。

FPSの数値によるON/OFF比較・ロード時frametime記録は未計測。カクつきなしはユーザーの体感確認であり、数値測定結果としては扱わない。
透明部だけの独立検証、P1から継続の法線陰影の独立判別も未完了。
P3のスキニング・アニメーションへは進まず、このP2結果を報告して停止する。
