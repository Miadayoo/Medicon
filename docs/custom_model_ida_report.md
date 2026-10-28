# CustomModel: IDA MCPで取得した描画入口とSDK情報

最新更新（2026-10-28 JST）: ユーザーのP0統合版確認後、P1のnative直方体が表示できた。
補間位置・body yaw・material key・texture copy/releaseを追加特定した根拠と実機結果は[P1報告](custom_model_p1_report.md)。
本書の「未確認」「未使用」は各調査時点の記録として残す。

最新の追加調査では、addPlayers候補も全視点でplayerが残るとの実機報告を受け、live登録を無効化した。
queueRenderEntitiesの近距離/ECS側2sigを新規取得し、元PEで各1件一致を確認した。
後続依頼で両候補を試験Hookへ接続してReleaseビルド済み。新候補の実機効果は未確認。
さらにユーザーから2本構成で3人称が消えたとの報告を受け、一人称も消す追加指示でDataDrivenRendererを併用する3本構成に変更・ビルドした。
統合版の実機効果は未確認。Show Debugは独立した初期OFF設定で、F8では診断窓を出さない。
詳細は[queueRenderEntities再調査](custom_model_p0_queue_candidates.md)。以下は従来の描画入口/SDK調査の記録。

2026-10-28（日本時間）。**P0用の描画入口の識別根拠・ABIと、P1周辺の9本の一意sigを取得した。**
本書は静的調査とSDK宣言の根拠を記録する。後続で[P0診断Hook](custom_model_p0_report.md)を実装・ビルドした。
P0は後続の実機報告で**現候補NG**。1人称の手が消える一方、3人称プレイヤー本体が残る。
この関数の静的な識別根拠を維持しつつ、目的の描画範囲を覆わないため実行時Hook対象から外した。P1候補は実行に使用していない。

## 対象と解析範囲

| 項目 | 値 |
| --- | --- |
| 対象 | `Minecraft.Windows.exe` / FileVersion `1.26.52.3` |
| 入力SHA-256 | `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942` |
| IDA ImageBase / SizeOfImage | `0x140000000` / `0x12C02000` |
| 接続 | ローカル IDA Pro 9.2 MCP、HTTP JSON-RPC |
| 実行時検証 | ゲームへのattach・inject・描画API呼出しは行っていない |

冒頭の最小概要取得がタイムアウトし、GUIも応答待ちになったため、以降は全体概要・文字列キャッシュ構築を使っていない。
保存済みIDBのコピーと別セッションでの全PEロードも待機したため中止した。
一時的に必要なコード/根拠文字列だけの約100KBの解析用PEを作り、元と同じVAに配置した各領域のSHA-256をIDA内で確認して限定解析した。このPEは解析用で、実行していない。

ユーザーによるIDA再起動後、**元のIDBから入力パス・SHA-256・ImageBaseをMCPで再取得し、一致を確認した。**
元のIDBは該当関数の先頭命令だけが定義された状態だったため、必要な関数の範囲だけに `plan_and_wait(start, end, false)` を適用し、本体・参照・sigを再取得した。
全体の `auto_wait()` や全バイナリの再解析は行っていない。現在は全体の自動解析を停止した状態。

## P0: 現行のDataDrivenRenderer描画入口

| 項目 | 取得結果 |
| --- | --- |
| 関数 | `DataDrivenRenderer::render(BaseActorRenderContext&, ActorRenderData&)` |
| VA / RVA | `0x14558D300` / **`0x558D300`** |
| PE unwind範囲 | `0x14558D300` ～ `0x14558E4E5`（末尾を含まない） |
| ABI | Windows x64、戻り値 **void**、RCX=`this`、RDX=`BaseActorRenderContext*`、R8=`ActorRenderData*` |
| vtable根拠 | コンストラクタ側の `0x14558BD3D` が `0x14E942B70` を設定し、そのslot 2（`+0x10`）は `0x14558D300` |
| 識別根拠 | `0x14558E479` がソース位置情報 `0x14E942BE0` を参照。そこに `DataDrivenRenderer.cpp` と完全な関数名文字列へのポインタがある |

ソース位置情報の内容:

```text
0x14E942BE0 -> 0x14EC11849
D:\a\_work\1\s\handheld/src-client/common/client/renderer/actor/DataDrivenRenderer.cpp

0x14E942BE8 -> 0x14EF9CBB0
virtual void __cdecl DataDrivenRenderer::render(BaseActorRenderContext &, ActorRenderData &)
```

非staticメンバの `this` を明示した、Hook用の機械ABI表現:

```cpp
void (*)(DataDrivenRenderer*, BaseActorRenderContext*, ActorRenderData*);
```

MCPのsig生成結果を、別途元PEの実行可能セクション全体で照合して **1件だけ** の一致を確認した:

```text
55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 01 00 00
48 8D AC 24 ? ? ? ? 44 0F 29 9D ? ? ? ? 44 0F 29 95 ? ? ? ?
44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ?
0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 4C 89 C6
```

上流の旧 `ActorRenderDispatcher::render`（7引数）とは別の関数である。
旧Hookの引数型をこの関数へ流用していない。

## 確認した読取位置

構造体全体を復元したものではなく、当該命令が実際に読む部分だけを記録する。
SDK宣言は [CustomModelRender.hpp](../include/medicon/sdk/CustomModelRender.hpp)。呼出しもメモリアクセスもまだ接続していない。

| オブジェクト | 位置 | 確認した内容・根拠 |
| --- | --- | --- |
| `ActorRenderData` | `+0x00` | actorポインタ。`0x14558D354` でR8をRSIへ保存し、`0x14558D4FF` で `[RSI]` を読む。 |
| `ActorRenderData` | `+0x10/+0x14/+0x18` | 位置のfloat 3本。`0x14558DAD9` ～ `0x14558DAE3` で読み、行列の平行移動に使用。座標系と補間の生成側は未確認。 |
| `ActorRenderData` | `+0x38` | 描画パラメータ側のポインタ。`0x14558D664`。上流旧ヘッダーの同位置にあるmodel ID/sizeとは一致しないため、その旧構造体を採用しない。 |
| `BaseActorRenderContext` | `+0x28` | `ScreenContext*`。`0x14558DA46/0x14558DA73` と別のactor描画関数 `0x145601A40` で同じ経路を確認。 |
| `ScreenContext` | `+0xB8` | 借用Tessellatorポインタ。`0x145601A40` 内の `*( *(context+0x28) + 0xB8 )` が `0x14560212D` の送信ラッパーへ渡る。 |
| `ScreenContext` | `+0x18` | DataDrivenRendererが行列操作に使う格納先ポインタ。その先の `+0x40` が64バイト行列のdeque操作に使われる。公開MatrixStack APIの完全復元ではない。 |
| `BaseActorRenderContext` | `+0xB0` | floatを読み、死亡/特殊姿勢のtick計算に使用。partial tickの候補だが、元の型と全用途は未確定。旧ヘッダーの `mFrameAlpha +0x10` は採用しない。 |

補間済みbody yaw、1人称/3人称、インベントリ描画contextの区別はまだ確定していない。
後続の実機報告でorig省略は1人称の手へ影響し、3人称本体は残った。P0を満たす入口としては不採用。

## P0不合格後の限定追跡

同じ元PEから`common/client/renderer/actor/`配下のソース名だけを検索し、legacyのDataDrivenRenderer.cpp以外に
`v2/DataDrivenRendererRegistry.cpp`と`v2/DataDrivenRendererRuntime.cpp`があることを確認した。
Runtimeのソース文字列VAは`0x14EC12E95`。RIP相対LEA候補を元PEで絞り、IDA MCPで
`0x145FECDA1` / `0x145FECE84` / `0x145FED00D`から各16命令だけをデコードして参照を照合した。
いずれもPE unwind範囲`0x145FE3770`～`0x145FF46FD`内だが、この大きな処理全体を代替Hookにはしない。
v2経路が3人称を担当する点、actor単位の入口、ABIと対象判定は未確定。
全体再解析やこの大きな関数全体の逆コンパイルは実施していない。IDA全体の自動解析は再び停止した。
根拠は`out/custom-model/ida/p0_v2_anchor_xrefs.json`と`p0_v2_anchor_nearby.json`。

## P1: 回収した描画API候補

全sig・一意性・根拠ファイルのハッシュは [custom_model_candidates.json](../data/signatures/custom_model_candidates.json) に保存した。
このJSONは調査用で、既存sigカタログへ自動登録されない。

| 役割 | RVA | 静的に確認できた内容 | 残る不確実性 |
| --- | --- | --- | --- |
| Tessellator begin | `0x59CA2E0` | nativeバッファの初期化、primitive設定、頂点数hint。RCX=オブジェクト、R8B=mode、R9D=count、入口RSP+0x28=byte。 | RDXの元の型は未特定。この本体では入力値を読まない。単純な「this + mode + count」の3引数関数とはしない。 |
| Tessellator vertex | `0x59CA7A0` | RCX=this、**XMM1/XMM2/XMM3=位置のfloat 3本**。`0x1459CA7D5/7D8/7DB` が各レジスタを保存。位置バッファへ12 bytes追加する。 | 逆コンパイルの仮引数にある整数RDXは正しいAPI宣言として採用しない。 |
| Tessellator end | `0x59CBD50` | RCX=tess、RDX=native出力領域、R8D=mode、R9=ラベルのpointer/length、stack=word。 | 出力Meshの完全型・所有権・modeの全値が未確定。 |
| Tessellator送信ラッパー | `0x46EEE30` | native Meshを終端処理で作り、`0x4B05040` で送信し、`0x4AFFD10` で破棄、Tessellatorの状態をリセットする。 | material/texture/第6引数の完全な意味と借用条件は未確定。 |
| Mesh即時描画の相当候補 | `0x4B05040` | 上記ラッパーから呼ばれ、texture入力を束ねて `0x4B04540` へ渡す。 | そのまま旧MeshHelpersのC++型を当てはめない。render context・texture binding型と描画フェーズの確認が残る。 |
| 行列dequeへの64 bytes追加 | `0x46FD480` | 行列16要素をコピーし、native dequeのサイズを増やす。 | メモリ確保があり得る。公開MatrixStack::pushとしての採用はしない。 |
| entity materialの取得候補 | `0x1EB5410` | `entity_alphatest` の文字列とハッシュを使ってnative materialを取得し、`this+0x18` に保持してそのアドレスを返す。 | このownerの安全な取得経路・寿命をまだ復元していない。 |
| material group lookup候補 | `0x52F1E00` | 呼出元は出力/グループ/keyをRCX/RDX/R8で渡す。native groupのvirtual関数を呼び、2ポインタの出力を返す。 | native keyの型とvirtual先の契約が未確定。DLLのSTLや架空のHashedStringを渡さない。 |

送信ラッパーの呼出元はPEから3箇所まで絞り、`0x145601A40` とcallsite `0x14560212D` をMCPで照合した。
この経路は既存のnative material/textureをその描画中に借用している。native ownerを別フレームへ生ポインタのまま持ち越す設計にはしていない。
同じ呼出元が `ScreenContext+0x83` を一時変更することも確認したが、フラグの意味は未確定であり、深度フラグだとは断定しない。

## color / UV / normal

頂点追加関数と既存メッシュ生成 `0x1460BB950` を限定して調べた。現時点では独立したsetter関数の確定ではなく、**インライン属性書込みと頂点側の読出し**が根拠。

| Tessellator内の位置 | 観測した処理 | 区分 |
| --- | --- | --- |
| `+0x178` と有効フラグ `+0x188` | 4 floatを16 bytes属性バッファへコピー。既存メッシュ生成に `(0,-1,0,0)` 等の面方向の値がある。 | normal属性の候補。シェーダで陰影へ使われるかは実機未確認。 |
| `+0x18C` と有効フラグ `+0x194` | 最初の2 float属性を8 bytesずつ追加し、そのUV boundsも更新。後ろに同形式の属性2組が続く。 | UV0の候補。追加のUV組の意味は未確定。 |
| `+0x1B0` と有効フラグ `+0x1B4` | 32 bit値を4 bytes属性バッファへコピー。既存描画には `0xFFFFFFFF` / `0x3FFFFFFF` の設定がある。 | packed colorの候補。チャンネル順序は未確定。 |

この観測値を不完全な巨大構造体としてハードコードしていない。
P1の描画コードに接続する前に、属性の生成元・native型・保存/復元範囲を確認する。

## 成果物と検証結果

| ファイル | 変更内容 |
| --- | --- |
| `include/medicon/sdk/CustomModelRender.hpp` | 根拠のあるP0用のopaque型、機械ABI、sig、最小限の読取位置。ゲームのSTLや未確定P1関数型は含めない。 |
| `data/signatures/custom_model_candidates.json` | MCPで生成した9sig、元PEで各1件の一致、静的/未確認区分、根拠ファイルのSHA-256。 |
| `docs/custom_model_sigs.md` | 初回不足一覧を今回のIDA調査結果へ更新。 |
| `docs/custom_model_p0_report.md` | 初回の「入口sig不足」という停止理由を後続更新で訂正。 |
| `out/custom-model/ida/` | MCP応答、アドレス付き逆アセンブル、逆コンパイル、領域照合manifestなどのローカル根拠。元バイナリ全体はリポジトリへ追加しない。 |

- **OK:** 再起動後の元IDBの入力ハッシュ・ImageBase一致。
- **OK:** 9sigすべて元PEの実行可能セクションで各1件だけ一致。
- **OK:** P0の関数名/void戻り値/3ポインタABIとactor読取箇所の静的な根拠を保存。
- **OK:** 最小SDKヘッダーのC++20構文チェック、SDK内sigとJSONの一致、根拠ファイルのハッシュ一致。
- **未実施:** P0の抑止範囲、他actorへの影響、OFF復元、1人称、インベントリプレビューの実機試験。
- **未実施:** P1の直方体、深度、ガラス/水、昼夜/洞窟、UV、material比較、法線の実機試験。
- **未計測:** FPSのON/OFF比較、追加描画時間、描画スレッドのメモリ確保。

P0の候補Hookを組むための入口sig/ABIは取得済みなので、この部分をユーザーへ再取得依頼する必要はなくなった。
次は既存HookManagerを使った呼出カウンタとローカルactorの照合、手動切替によるorig省略の検証となる。
P1の全完了条件を満たした時点で停止して報告し、ユーザーの確認前にP2へは進まない。

## 一時ファイル

別セッションで使った自分のIDA workerはすべて終了した。こちらからユーザーのIDAプロセスを終了する操作は行っていない。
`out/custom-model/ida/session/` と `raw/` の大きな使い捨てコピー/部分DBは、一括削除が自動承認レビューのポリシーで拒否されたため残っている。
削除を別手段で回避していない。元のゲーム実行ファイルとダウンロード素材は変更/削除していない。
元IDBには再起動後に指定範囲のコード解析を追加したが、独自の名前/型の適用やバイナリパッチは行っていない。
