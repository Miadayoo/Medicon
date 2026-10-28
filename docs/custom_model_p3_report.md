# CustomModel P3 スキニング・アニメーション

2026-10-28 JST。「p3やって」に基づく実装。**初版P3はReleaseビルド・関連12テスト成功。その後の向き・一人称の修正はソース反映済み。ユーザーの「完成してもビルドはしなくていい」に従い、修正版のビルド・追加テスト実行・実機確認は行っていない。**

## P3試験後の修正（未ビルド）

初版の実機試験について、ユーザーから「動きとかカクつきはない」、ただしモデルが後ろ向きで、一人称ではモデルに視界を塞がれるとの報告を受けた。

- 向き: model.jsonのyawOffsetをnativeの`180-bodyYaw`へ加算する。旧処理はさらに180度を引いていたため、設定yawOffset=180のSprinterが逆向きになった。`model_world_matrix()`で補正し、モデル設定ファイルを編集せず向きを直す。
- 一人称: nativeの`game_thirdperson`を現在のrender callback内で読み、Custom modelの全身描画を三人称の前後2視点に限定する。Animation=OFFの静止モデルにも適用。一人称でもnative mesh/textureの準備を進め、視点変更でreadyを失わない。
- 手: ユーザーの追加指定により、一人称でもカスタムモデルの腕・手を描く。FBXの肩以下の骨とskin weightで腕の三角形だけを選び、元のPNG/UVで描く。頭・髪・胴体・脚は一人称の描画に入れない。カスタム腕の描画成功後にDataDrivenRendererでバニラの腕を抑止し、骨/姿勢/描画準備が不足する間はバニラの腕へ戻す。
- 判定失敗: 全身カスタム描画とバニラ全身の抑止を控える。F5の押下回数、カメラとプレイヤーの距離からの推測は使わない。Show Debugに視点と手の扱いを表示する。

`CustomModelView.hpp/.cpp`を追加し、既存SignatureRegistryで一意解決したnative関数を読み取りlayoutの検証に使用。新しいnative hook・native関数呼出し・ゲーム状態への書込みは追加していない。
IDA根拠は[シグネチャ報告](custom_model_sigs.md)。元PE上の追加3パターンが各1件で、ヘッダーとJSONのbytesが一致することを確認した。

回帰テストのソースに、四方位の向き、first/back/front設定、option override、循環/不正設定の拒否、一人称でready維持、三人称への復帰、手を残しつつ両world queueを抑止するケースを追加した。**これらは未ビルド・未実行。初版の12テスト成功を修正後の成功とは扱わない。**

### カスタム腕と表示名の追加変更（未ビルド）

`src/custom_model/FirstPerson.cpp`で、肩以下の子孫骨への合計weightが各頂点で70%以上あり、3頂点とも同じ腕に属する三角形だけを選ぶ。
腕以外の残余weightを取り除いて再正規化し、独立したarm-only頂点・material batchをworkerで作る。rig cache hitでも公開前に導出するため、既存cacheの削除やFBXの再書出しは不要。
Animatorのworker確保バッファで一人称用の腕姿勢をskinし、カメラ位置・補間済みhead yaw/pitchに追従する行列で描く。攻撃phase・使用状態に対応し、Animation=OFFなら腕の静止姿勢を維持する。
三人称への切替時は全身用poseを再初期化する。一人称で全身の静止meshへfallbackすることはない。native hook/sigは前項から追加していない。

実モデルの既存rig cacheをPythonで読み、同じ骨/weight選択と行列計算を独立に確認した。全13,046頂点から腕2,427頂点、左1,875・右1,879三角形を抽出。
idle/attack/useの各姿勢で左右の手がカメラ前方にあり、FOV70度・16:9の計算上の画角内に手の全1,794頂点が入る。監査結果は`out/custom-model/first-person-audit.json`。
これは実データの数値監査であり、C++コードの実行やゲーム内の描画確認ではない。壁との交差や見た目の最終調整は実機確認が必要。

表示名を`CustomModelProbe`→`CustomModel`、module IDを`custommodelprobe`→`custommodel`へ変更した。旧IDのコマンドlookupと設定読込を互換対応し、保存時は新IDを使う。
最新の訂正に従い、設定名は`Material`、その先頭の選択肢`entity_alphatest`の表示名を`Neon`とした。設定キー`material`、選択index、native描画materialへの対応は引き継ぐ。
回帰テストのソースに腕以外の三角形除外、UV/PNG対応、weight再正規化、カメラ相対配置、攻撃/使用、視点切替時のbuffer再利用、設定移行・表示名の確認を追加した。これらも未ビルド・未実行。

### Loader経由のモデル同梱修正（未ビルド）

外部の`models/neon-t-pose`を読む構成だったため、Core DLLだけを配信するLoaderではモデル資産を届けられなかった。
`assets/models/neon-t-pose`へ元のFBX・model.json・描画に必要な3枚のdiffuse PNG（合計4,420,804 bytes）を取り込み、`cmake/EmbedModel.cmake`でSHA-256を固定したWindows RCDATAとしてCore DLLへ同梱するよう変更。
`BundledModel.cpp`は自身を含むmoduleからresourceを取得するため、配信後の`MediconCore.dll`への改名にも依存しない。
外部モデルの読込失敗時はModelRegistryのworkerで`%LOCALAPPDATA%/Medicon/cache/models/neon-<pack SHA-256>`へ検証・展開して既存のFBX/rig/texture処理へ渡す。
有効な外部モデルは優先し、展開済みファイルはSHA一致なら再利用、破損時は置換する。ファイルI/Oや解析をrender callbackへ追加していない。
現在のrendererが使わないnormal/roughness/metallic PNGや生成cacheは同梱しない。MaterialのNeonは既存alpha-test shaderの表示名であり、モデル選択ではない。

リソースをリンクしたテストEXEに、実Neon資産の展開・読込、一人称腕の生成、cache再利用・破損修復、外部資産がないLoader相当のfallback、有効な外部override優先の回帰テストを追加した。
**ビルドと新規C++テスト実行は指示に従い未実施。** CMakeの資産生成スクリプト単体で5ファイルのSHA/sizeとresource一覧生成を確認した（pack ID: `39aa4cbb78a0b6e13be95433967dce10ec34e79cc660e5bee92cdfe32f20ee24`）。同梱する5ファイルだけを検証用フォルダへコピーし、既存のMediconModelInspect.exeで19,952三角形・3材質・218骨・13,046 skin頂点の読込を確認した。結果は`out/custom-model/bundle-validation/import.json`。これは同梱元資産の検証であり、新しいDLLのresourceリンク・展開処理の実行確認ではない。
配信済みCore・読み込み済みDLLは更新していないため、Loaderへ反映するには今後この変更を含むDLLをビルドし、新しいCoreリリースとして配信する必要がある。

## 試験DLL・操作

- 初版: `D:\code\Medicon\out\build\local\bin\Medicon-P3.dll`。初版作成時の通常出力Medicon.dllと同一bytes。今回の修正を含むDLLは作成していない。
- SHA-256: `cb8167b54e6eb83dba120dbafeb62817e4319fe2c44d8a6b6a0d92bbe6cfa80d`。
- Minecraft再起動後、このDLLだけを読み込む。CustomModelProbeのMode=Custom model、Animation=ON、F8=ON。
- Animation=OFFならP2の静止姿勢。F8=OFFで描画・バニラ抑止とも停止。Show Debugは独立設定を維持。
- Show Debugに状態名、入力可否、骨/skin頂点数、CPU skin時間、native draw時間、bulk/fallback batch数を表示。

## 資産と実装内容

指定のSprinter.fbxには218ノード、3つのskin mesh、各137 clusters、アニメーションクリップ0本がある。
別のアニメーションFBXはないとユーザーが回答したため、現在の骨を使う基本動作を生成する方針で実装。
idle/walk/run/sneakIdle/sneak/fall/swim/sit/glide/sleep/attack/useの12種をworkerで30HzのTRSに焼く。
正式なFBX clipが同梱されていれば、model.jsonのclips名または状態名で選び、ufbxで30Hzにサンプリングする。
見つからないclipは生成した基本動作へ戻し、diagnosticにgenerated数を出す。

骨名はMixamoに加え、この資産のSplitter/Spine1/Neck/Head、L_/R_Shoulder、Elbow、Hip、Kneeを実在名で対応付けた。
model.jsonのbones指定があれば優先する。不明な明示指定、非対応のscale inheritance、壊れた資産は読み込み失敗としてバニラを維持する。

| ファイル | 内容 |
| --- | --- |
| `include/medicon/custom_model/Animation.hpp` | TRS、骨・bind・4ウェイト、clip、入力、worker確保のAnimationInstance |
| `src/custom_model/Animation.cpp` | affine/quaternion、SSE2 LBS、normal逆転置、状態ブレンド、spineマスク、head/neck加算 |
| `src/custom_model/FirstPerson.cpp` | workerの腕メッシュ抽出・元texture/UV維持、一人称専用pose・スキニング |
| `src/custom_model/RigImporter.cpp` | 骨階層・inverse bind・top4正規化、30Hz bake、水平root motion除去、基本clip、rig cache |
| `include/medicon/custom_model/ModelAsset.hpp`, `src/custom_model/ModelAsset.cpp` | 骨データとruntime bufferをworkerで準備し、既存atomic公開へ接続 |
| `include/medicon/sdk/CustomModelAnimation.hpp`, `src/sdk/CustomModelAnimation.cpp` | callback内のread-only native入力、native確保済みTessellator配列への一括転送 |
| `include/medicon/sdk/CustomModelBox.hpp`, `src/sdk/CustomModelBox.cpp` | P2のscope/状態復元/材質/PNGを共有し、P3更新と3材質batch描画を追加 |
| `include/medicon/sdk/CustomModelP3Patterns.hpp`, `data/signatures/custom_model_p3.json` | swing状態の読み取り根拠となる2本の一意profile |
| `include/medicon/modules/CustomModelProbeModule.hpp`, `src/modules/CustomModelProbeModule.cpp` | Animation設定、P2静止表示との切替 |
| `src/platform/AutoOverlay.cpp` | reader profile初期化と診断表示。native hookの追加はなし |
| `tools/custom-model-inspect.cpp` | 実資産の骨・clip/cache、静止姿勢誤差、CPU benchmark出力 |
| `tests/custom_model_animation_tests.cpp` | SIMD対照、top4、状態/上半身、heap再利用、native入力/bulk、authored clip/root motion/cache検証 |
| `CMakeLists.txt` | Animation/RigImporter/readerとテストのビルド接続 |

ufbxのclusterはgeometry-to-boneのbindを持ち、頂点のウェイトは影響順に並ぶ。上位4本を取り、切り捨て後に正規化する。
参考: [ufbx deformers](https://ufbx.github.io/elements/deformers/)、[animation evaluation](https://ufbx.github.io/elements/animation/)。

## 動作とスレッド

- bind位置/normalをworld-metre空間にそろえ、`animated bone world × inverse bind`を使う。normalは逆転置後に正規化。
- 同じlogical vertexとnormalを共有する頂点をまとめ、59,856描画頂点を13,046 skin頂点に削減。UV seamは描画用indexとUVの別保持で維持。
- 移動速度からwalk/runをブレンド。sprint/sneak/ground、ride/swim/glide/sleepに状態を切り替える。
- 状態切替は150msでcrossfade。下半身は移動を続け、攻撃/使用はspine以下のmaskに重ねる。上半身の切替/終了もfadeする。
- 攻撃はゲームのswing値をnative render alphaで補間し、そのphaseをclipへ直接渡す。独自の攻撃タイマーは使わない。
- 頭と体のyaw差は±65度、pitchは±50度に制限してneck/headへ分配。被ダメージ時に赤くし、dead flag時には倒れる回転を加える。
- ride/swim/glide/sleepの専用clipがない場合は、生成姿勢や全身回転で代用。
- world位置・body yawはP2と同じnative補間済み値。自前で位置を再補間しない。
- 48ブロックを超える場合はスキニングを省く。対象はP2と同じローカル1体。
- ファイルI/O、FBX処理、clip bake、cache、全pose/palette/output/batchバッファの確保はworkerのみ。公開後のAnimator/Skinnerで再確保しない。
- actorは毎回既存の検証済みgetterで取り直す。native actor/context/pool pointerをinstanceへ保存しない。コピーしたActorUniqueIDとworld識別値で状態を区別する。
- `.rig.mmdl`には骨階層、bind、4ウェイト、part index、TRS clipを保持。FBX/設定SHA、format version、body SHAで無効化/破損検出。P2の静止geometry cacheと共存。

## native根拠・追加sig

対象PEはP2と同じ1.26.52.3、SHA `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`。
既存P0/P1/P2のversion/profile gateとSignatureRegistryを使う。追加2本は**読み取りlayoutの確認用scanのみ。書き込み関数を呼んだりhookしたりしない。**

| VA (RVA) | 根拠と用途 |
| --- | --- |
| `1447722D0` (`47722D0`) | native swing tick更新。+434のswinging、+410のtick、durationで割ったfloatを+40Cへ格納。bounded end `144772331` |
| `142077CC0` (`2077CC0`) | tick開始時に+40Cを前回値+450へコピー。bounded bodyはPE unwind範囲、必要なprefixを確認 |

64-byte profileは元PEの実行可能section全体に各1件。sig bytesはJSON。
上流OffsetProviderの古いold swing +43Cは使用せず、今回確認した+450を読む。
骨・actor状態読み取りのために新たなnative関数は実行しない。

EnTTのregistry/dense-map/sparse-page layoutは既存`GameplayMemory`の検証済み実装を共有。
component hashと値のlayoutはpinned Solsticeの`EnttHashs.hpp`、`Actor/Components/{ActorDataFlag,StateVector,ActorRotation,ActorHeadRotation,ActorUniqueID,MobHurtTime}Component.hpp`、ground/sleep/dead flagを参照。
callback内でpoolを探し、値を小さなstructへコピーする。assure/emplace、actor書き込み、packet送信は行わない。

一括転送はP1で確認した`1459CA7A0`のnative vertex bodyが根拠。
position vector+10（stride12）、normal+28（stride16）、color+70（stride4）、UV0+A0（stride8）、vertex count+298、bounds+130/+13C。
native beginのcount hint後、native seed vertex 1個で各配列を確保する。begin/end/capacityとサイズを検証してから全体を埋める。
容量が一致しなければ従来のnative vertex経路へ戻し、診断でfallback数を示す。native vectorの構築・破棄はゲーム側。
材質ごとにまとめるため、P3のnative vertex呼び出しは通常3回/frame。59,856回/frameにはしない。
描画属性・bounds・world行列を保存復元し、texture/metadataの所有権はP2のまま。

## 計測・テスト

- ReleaseとDLL lifecycleを含む関連12テストが全件成功（7.28秒）。
- 10,000頂点・4 influencesのSIMD LBS: 平均`0.195397 ms`（200回、ゲーム外）。scalar対照との差を検査。
- 実FBXのAnimationInstance全更新: 平均`0.189054 ms`（218ノード・13,046 skin頂点、200回、ゲーム外）。別回は約0.25ms。
- bind姿勢とP2の最大position差: `4.76837158203125e-7`ブロック。
- Animator/Skinnerの連続更新で自前heap allocation 0をunit testで確認。
- authored FBXのtranslation clipを30Hzにbakeし、水平root motionの除去を確認。
- 壊れたrig cache、5 influencesからtop4への切捨て/再正規化、spine mask、状態遷移、入力のstale entity拒否、bulk容量不足時の非破壊拒否を検証。

CPU benchmarkはnative入力読み取り・描画・GPU送信を含まない。**追加描画時間全体の0.3ms達成や、nativeエンジン内部のheap allocation 0を示す結果ではない。**
native Tessellator/Mesh内部の確保は既存ゲームAPIへ委ねており、自前バッファの再利用と区別する。
ゲームでのON/OFF FPS・native draw時間、動きの自然さは試験DLL再読込後に確認する。

## 完了条件・残る確認

| 条件 | 現状 |
| --- | --- |
| 歩く・走る・しゃがむ・ジャンプ | 状態/生成clip/描画を実装、unit成功、実機確認待ち |
| 攻撃と視点移動 | native swing phase/頭・首加算を実装、実機確認待ち |
| 読み込みとバッファ再利用 | worker公開、cache、unitで確認 |
| FPS/追加描画時間 | 単体CPU計測のみ。実機測定待ち |

オフスクリーンのfrustum判定、UUID→複数player instance管理は未実装。P3はlocal actorのみで、コピーしたworld/ActorUniqueIDを使い、native actor pointerを保持しない。
三人称カスタムモデルの手持ちアイテムはP4、inventory preview等はP5、他プレイヤーへの適用はP6として進めていない。一人称の全身除外とカスタムモデルの腕・手の表示は、追加指示に従ってソースに反映した。
正式なアニメーションFBXがないため、基本clipの動作品質は実機で調整する必要がある。追加ファイルの提供は現在不要。
