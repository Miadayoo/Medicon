# CustomModel: Chocolat v1.02

2026-10-28 JST。ユーザー指定のフォルダから標準`FBX/Chocolat.fbx`と付属PNG 5枚を追加した。原本は変更していない。

## 操作

CustomModelの`Mode = Custom model`、`Model = Chocolat`で選択する。`Model = Neon`へ戻せる。
既存設定にはModel項目がないため、従来どおりNeonが初期値になる。Material/Animation/Show Debug/F8の意味は引き継ぐ。
モデル切替はworkerが担当し、読込中・native mesh/PNG準備中はバニラを表示する。
読み込んだ2モデルのCPU資産は停止まで保持し、切り戻し時のFBX再解析とrender threadでの大きな解放を避ける。

## 同梱・読込

- `assets/models/chocolat-v1.02/`: 元FBX、PNG 5枚、model.json。
- FBX SHA-256: `b4e05dc30e9e139418ce054f6b65bb6332849b201c2b546aa988e74e43300fdb`。
- `cmake/EmbedModel.cmake`でNeonとChocolatを1つのRCへ生成し、Core DLLへ同梱する。Loader側の追加配布処理は不要。
- 外部`models/chocolat-v1.02`が有効なら優先し、なければDLLから`%LOCALAPPDATA%/Medicon/cache/models/chocolat-<pack SHA-256>/`へ展開する。
- model.jsonのmaterial名→PNG対応で古いPSD参照を解決。モデルフォルダ外への参照は拒否する。
- スケール1.5、身長約1.80ブロック。明示した骨の役割を使い、Neonと同じ基本動作・一人称腕抽出を行う。
- 衣装ごとに重複するbone/inverse-bindをworkerで統合し、5,380 bindingから288へ削減。既存の上限は拡大しない。
- ufbxの`UFBX_INHERIT_MODE_HANDLING_COMPENSATE`でスケール継承を通常のTRSへ変換。髪のゼロ法線はufbxのtopology/smoothingに従ってbind形状から補う。静止/rig cache versionを更新して再生成する。
- native resourceの世代を比較し、切替時は描画/texture requestの両lock下で旧meshとPNGを解放して準備をやり直す。world scopeで取得した同一のCPU資産を行列計算と描画へ使う。

## 検証

読み込み用CLIと関連テストEXEだけをビルドした。**ゲーム用Medicon.dll・MediconLoader.dllの再ビルド、配信、inject、実機描画確認は行っていない。**

- 実資産: 83,290三角形、5材質、73 mesh chunks、290骨、96,654 skin頂点。
- 一人称用: 18,613頂点、14,334三角形。idle/attack/useで手がカメラ前方にあることを検査。
- 独立した数値監査: FOV70度・16:9で手の頂点の約96–98%が画角内。腕の付け根は通常のview modelとして画面外/near clipへかかる。
- bind姿勢と静止モデルの最大position差: `3.57627868652344e-7`ブロック。
- ゲーム外のAnimationInstance更新: 約1.26 ms/frame（200回、96,654頂点。native描画/GPU時間を含まない）。Neonより頂点数が多いため、実機FPSは別途確認が必要。
- 関連テスト: `custom_model_animation`、`custom_model_asset`、`custom_model_box`、`runtime`。材質PNG対応とパス拒否、実モデルのresource展開、骨/腕生成、切替/高速切り戻し、旧native handleの解放、新しい設定の既定値を検査。
- 検証ログ: `out/custom-model/chocolat-validation/import.json`、`first-person-audit.json`。
- 本体のAutoOverlay.cppと同梱RCのobject生成も成功。DLLのlinkは行っていない。

Unity/lilToon専用shader、PhysBone、表情/衣装切替を再現する変更は含まない。付属diffuse PNGと既存native材質による表示。
