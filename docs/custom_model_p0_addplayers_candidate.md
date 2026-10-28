# P0: 3人称の候補として取得したaddPlayersコールバック

**後続更新: 実機不合格。** ユーザー報告では全視点でplayerが残った。
限定IDA調査で、このcallbackは近距離collectorとは別の遠距離追加経路と分かった。
ソース側のlive登録を無効化した。新しい2候補は[queueRenderEntities再調査](custom_model_p0_queue_candidates.md)。
以下の「未実装」「一時停止」は初回取得時の履歴。その後に試験実装・Releaseビルド・実機試験を行っている。

2026-10-28 JST。IDA MCPで限定取得。**プレイヤーごとの描画候補を集める経路の静的候補であり、3人称のローカルプレイヤーを消せると確定したsigではない。**
ユーザーの指示に従い、sigの記録と検証の後で作業を一時停止する。Hook実装・実機試験・P1は行っていない。

対象はMinecraft.Windows.exe FileVersion `1.26.52.3`、SHA-256
`47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`。

| 項目 | 静的結果 |
| --- | --- |
| 候補 | `ActorRenderCandidates::addPlayers` 内のlambdaに対応する呼出し先 |
| VA / RVA | `0x14480E6C0` / **`0x480E6C0`** |
| 範囲 | `0x14480E6C0` ～ `0x14480EE9E`（終端を含まない） |
| 機械ABIの観察 | RCXはcallback object、RDXはplayer/actorに見えるポインタ。正常終了経路はAL=1。C++元型・完全なcallback契約とthis調整は未確定。 |
| sig | [候補JSON](../data/signatures/custom_model_p0_addplayers_candidate.json) の`candidate.pattern`。111 bytes、元PEの実行可能領域で**1件**、IDAから取得した先頭111 bytesと一致。 |
| 実行時状態 | 未登録・未実行。以前の不合格DataDrivenRenderer Hookも無効化されたまま。 |

## 根拠

1. `0x14E8E3F30`の関数表は`+0x10`に`0x14480E6C0`、`+0x18`に型を返す小関数`0x14480EF60`を持つ。
2. 型取得関数は`0x151917F60`を返し、その型名は`.?AV<lambda_1>@?0??addPlayers@ActorRenderCandidates@@QEAAXAEAVILevel@@AEBUDimensionType@@AEBVVec3@@M@Z@`。
3. `0x1446929A9`がこの表をcallback objectへ設定し、`0x1446929FD`がそのcallback objectを渡してvirtual呼出しする。
4. `0x14480E6F0`がRDXを保持し、`0x14480E6FA`で`[RDX+0x218]`、`0x14480E776`で`[RDX+0x1D8]`を読む。後者は既存のsolstice `Actor_mLevel`参照と一致するが、型の完全な証明ではない。
5. 条件判定の後、`0x14480E8E3`～`0x14480E8F4`はキャプチャされたvectorにactorらしいポインタを追加し、正常終了点`0x14480EE46`でALを1にする。途中に関連エンティティを走査する処理もある。

同じ調査では、v2側の`0x145FE3770`が複数エンティティを扱うことを示す`separateEntityStreams`の文字列と、
`0x14470EDA8`からの呼出しも確認した。ただしこの大きな関数をそのままローカル1人のHookに使わない。
`separateEntityStreams`の6引数の文字列はその関数内のinline処理に由来する可能性があり、`0x145FE3770`自体の完全なABIとして採用しない。

## 未確認

- ローカルプレイヤーの3人称描画でこのcallbackが呼ばれるか、RDXが既存`is_local_player`と同一のポインタか。
- 省略した場合に体・防具・手持ち・マントがどう変わるか。他プレイヤー、乗り物、同乗者に影響しないか。
- AL=1がcallback呼出元にとって列挙継続を意味するか。1人称の腕、インベントリのプレビューへの影響。
- これは描画候補の収集段階で、P1でゲーム描画contextを受け取り直方体を描く入口は別途必要。

上記を確認する前に、推測した型やオフセットでHookへ接続しない。
IDAで全体自動解析は行っていない。限定した関数と呼出し箇所のデコード・データ参照の照合を行った。
追加の監査出力は`out/custom-model/ida/p0_addplayers_lambda_evidence.json`、
`p0_addplayers_outer_evidence.json`、`p0_addplayers_signature_validation.json`にある。
