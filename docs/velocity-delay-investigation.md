# Velocity Delay 調査

調査日: 2026-10-30（日本時間）。対象: The Hive / ユーザー指定の Flareon prediction、Minecraft Bedrock Edition。要望は「KB を約8秒保留し、その後に受ける」。現行 Medicon の対象 native profile は `1.26.52.3`。以下は実装前の調査記録で、後続の試作については末尾の追記を参照。

## 調査結果

KB を遅らせる実装例は存在する。ただし、現在の The Hive / Flareon に対して「8秒遅らせても検知・補正されない」と確認できる一次資料や実測結果は得られなかった。遅延処理を実装できることと、そのサーバーで受理されることは別の確認項目である。今回、Delay モードのコード追加は行っていない。

Flareon の現行サーバー実装・KB 判定の上限・設定値は、この調査では確認できなかった。以下の Boar / Grim の説明を Flareon の内部仕様として扱わない。

## 公開実装で確認した方式

| 方式 | 確認した実装 | 8秒への適用について |
| --- | --- | --- |
| KB の受信処理だけ保留 | LiquidBounce の共通 Delay はローカルプレイヤーの velocity packet を保留し、後から通常処理へ戻す。設定範囲は0–40 tick。 | 実装例であり、現行 Flareon への有効性の証明ではない。 |
| KB と確認用パケットを保留 | FDPClient の Delay は velocity と transaction を同じ保留処理へ入れる。 | Java 版の実装。MCBE の確認パケットや順序へ、そのまま対応付けることはできない。 |
| 受信ストリームをまとめて保留 | LiquidBounce の Lag は被弾を契機に受信パケットをキューへ入れ、後で解放する。KeepAlive は除外し、設定範囲は1–20 tick。 | KB 以外の受信にも影響する。8秒を正常に扱えるという根拠にはならない。 |
| ping に干渉 | pinned Solstice の Flareon 向け Disabler には PingSpoof / PingHolder がある。 | 送信直前の低層パケットを扱う処理であり、8秒後に KB を再適用する実装ではない。 |

出典: [LiquidBounce 共通 Delay](https://github.com/CCBlueX/LiquidBounce/blob/79c8ee07138431a2a4f5fa0b346b4df942ab5ca9/src/main/kotlin/net/ccbluex/liquidbounce/features/module/modules/combat/velocity/ModuleVelocity.kt)、[LiquidBounce Lag](https://github.com/CCBlueX/LiquidBounce/blob/79c8ee07138431a2a4f5fa0b346b4df942ab5ca9/src/main/kotlin/net/ccbluex/liquidbounce/features/module/modules/combat/velocity/mode/VelocityLag.kt)、[FDPClient Velocity](https://github.com/SkidderMC/FDPClient/blob/main/src/main/java/net/ccbluex/liquidbounce/features/module/modules/combat/Velocity.kt)。前二者は調査時の commit に固定した。FDPClient のリンクは可変 branch。

Solstice は Medicon と同じ参照 commit `1d7808f52ef4ba6e34a7fbcc65294907a7761611` を確認した。Flareon というモード名や遅延設定が存在することから、現行 Hive の KB 検知を回避できるとは判断できない。低層の ping timestamp、ゲーム内の確認応答、実際の packet 到着時刻は区別する必要がある。[Disabler.cpp](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/Features/Modules/Misc/Disabler.cpp)、[Disabler.hpp](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/Features/Modules/Misc/Disabler.hpp)

## 予測型 AC が遅延をどう扱うか

MCBE の公開例である Boar は、自分宛ての SetEntityMotion に VelocityAck を付け、対応する確認応答が受理されたときに予測側の確定 velocity を更新する。この実装では motion packet の tick を0に設定している。Flareon も同じ処理だとは確認できていない。[PlayerVelocityPackets](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/packets/player/PlayerVelocityPackets.java)、[BoarDefaultAcknowledgments](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/ack/BoarDefaultAcknowledgments.java)

Boar は NetworkStackLatency を確認に利用する。さらに、後の応答が届くと、それ以前の保留確認も処理する。特定の確認だけを遅らせれば独立にKBを保留できる、とはこの実装からも言えない。[NetworkLatencyPackets](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/packets/other/NetworkLatencyPackets.java)、[LatencyUtil](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/util/LatencyUtil.java)

Java 版の比較例である Grim は、velocity の前後に transaction を置き、後の確認が進むと KB が到着したものとして予測結果と照合する。これは「後で100%受ければ、その前の動きも正常になる」という判定ではない。[KnockbackHandler](https://github.com/GrimAnticheat/Grim/blob/2.0/common/src/main/java/ac/grim/grimac/checks/impl/velocity/KnockbackHandler.java)

以上からの推論: KB だけを長時間保留し、確認応答と移動を通常どおり進める方式には、AC が期待する KB と報告位置が食い違う可能性がある。通信遅延として扱わせる候補もあるが、確認の順序、独立した時間監視、補正、切断条件まで整合する必要があり、8秒の成功は未確認である。

## 8秒と Bedrock の tick / rewind

20 tick/s なら8秒は160 tick。Mojang の設定資料には rewind 履歴40 tickという既定例があるが、これは Hive の設定値でも、Hive の許容遅延上限でもない。[Mojang AntiCheatServer](https://github.com/Mojang/bedrock-protocol-docs/blob/242bf15706a936ae890c6a7cf89e343a313f7f54/additional_docs/AntiCheatServer.md)

Mojang の `automated/1.26.52` branch の schema は、SetActorMotion に motion だけでなく PlayerInputTick が含まれることを示す。schema 自体の version metadata は `1.26.52-beta.3` であり、これだけで retail の native ABI を証明するものではない。tick は処理済み入力との対応に使われ、0も許容される。[SetActorMotionPacketPayload](https://github.com/Mojang/bedrock-protocol-docs/blob/242bf15706a936ae890c6a7cf89e343a313f7f54/json/SetActorMotionPacketPayload.json)、[PlayerInputTick](https://github.com/Mojang/bedrock-protocol-docs/blob/242bf15706a936ae890c6a7cf89e343a313f7f54/json/PlayerInputTick.json)

Bedrock の予測補正には過去 tick へ戻って再計算する経路がある。したがって、packet を8秒後に処理することと、その時点から普通のKBを開始することは同じとは限らない。実際の受信 tick、履歴範囲、適用経路の確認が必要である。これは公式の一般説明からの推論であり、Hive の実測ではない。[Mojang PlayerMovementOverview](https://mojang.github.io/bedrock-protocol-docs/guides/player-movement-overview/)

なお、Mojang の同 branch では NetworkStackLatency は旧 telemetry 用として deprecated と記載されている。一方、上記の Boar は独自の確認に利用する。公式の deprecated 表記から、独自サーバーでも使われないと推定してはいけない。[Mojang NetworkStackLatencyPacket](https://github.com/Mojang/bedrock-protocol-docs/blob/242bf15706a936ae890c6a7cf89e343a313f7f54/docs/NetworkStackLatencyPacket.html)

## Medicon 側で追加前に必要な確認

現行の `hook_motion_dispatch` は呼び出し中だけ有効な借用 packet reference を受ける。`VelocityModule::process_motion` は、その場でキャンセルまたは倍率変更を判定する。遅延キュー、packet の所有権、後から native 処理へ戻す経路、tick の記録はまだ実装していない。

借用ポインターを保存して8秒後に呼び直す設計は、packet や session の寿命を越える可能性がある。motion の3値だけを保存して速度へ直接書く設計も、native の tick / rewind 処理と同じとは限らない。待機に Sleep を使うと呼び出し元を停止するため、遅延する機能には非ブロッキングな保留処理が必要になる。複数回の被弾、補正、死亡、world/session の変更、OFF時の処理も定義する必要がある。

受理を判断するために不足しているのは、現行の対象 session における次の観測である。

1. 受信KBの monotonic 時刻、runtime ID、motion、tick。
2. PlayerAuthInput の tick、position、position delta と補正packetの対応。
3. 実際に使われる確認応答、応答の順序と時間。表示pingだけでは判断しない。
4. 保留中・解放後の位置補正、移動・攻撃の処理、切断、利用可能なサーバー側の検知記録。

遅延処理自体は、再現可能なテスト環境で 0 / 50 / 100 / 250 / 500 / 1000 / 2000 / 4000 / 8000 ms、連続被弾、world変更などを比較して検証できる。その結果だけで、異なる Flareon の現行設定に通るとは判定できない。また、短時間キックされなかったことから検知が一切ないとは判断できない。

## 現時点の判断

候補は通信の確認順序も考慮した遅延処理。ただし、Hive / Flareon で8秒を許容する証拠は不足している。「8秒後にKBを適用する機能」と「現行Hiveで検知されず通常プレイできる機能」を分けて評価する必要がある。

古い FlareonKickCodes は2022年に archive され、README も不正確な点があると明記するため、現行 Flareon の判定根拠には採用しなかった。[FlareonKickCodes](https://github.com/disepi/FlareonKickCodes)

この調査では Hive への接続・注入・遅延試験は実施していない。作成したのは本調査記録のみで、モジュール・hook・ビルド成果物は変更していない。

## 後続の試作実装（2026-10-30）

調査後の「とりあえず実装してみて」という指示により、Velocity に Delay を追加した。
0–10,000 ms（初期値8,000）で、KBの値をコピーして保留し、期限後のゲームtickで
現在のローカルプレイヤーの velocity へ適用する。通常の移動入力は保留しない。
native packet の再生、元tickでのrewind、ping/確認応答の操作はこの試作には含まれない。
前述の通信順序を含む遅延方式が実証された、あるいはHive/Flareonへの適合性を確認したという意味ではない。

キュー上限は256件。1tickに最大1件を適用し、OFF、Delay以外への切り替え、
フォーカス・入力条件の喪失、死亡、actor/worldの変更、snapshotのclearで保留を破棄する。
詳細は [gameplay-modules.md](gameplay-modules.md) と [SDK検証記録](gameplay-sdk-validation.md) を参照。

## 位置戻し報告後の変更（2026-10-30）

最初の試作で保留中／適用時に位置戻し・キックが起きるとの報告を受け、
Sync Replies（初期値ON）を追加した。送信NetworkStackLatencyを、直前の保留KBの
適用成功とその次のゲームtickまで待たせ、後続の応答も追い越さないようFIFOで返す。
通常の移動入力と他の受信packetは保留しない。OFF時は保留応答を次の有効tickで返し、
world/actorが変わった応答は再利用しない。

これはBoarなどで確認した「確認がKBを追い越す」仕組みへの試作対処であり、
Flareonでその機構が原因だったと確定したものではない。自然な通信遅延はKB専用ではなく、
同じ確認に紐付く別の同期にも影響する可能性がある。HiveがRakNetや別の基準で
時刻を追っている場合、NetworkStackLatencyだけではその状態を再現できない。
8秒を許容するか、位置戻しが解消するかも実サーバーでは未確認。

`inject.log` の `Velocity Delay:` にKB・確認の観測／保留／適用／送信と失敗数を記録する。
手元で確認できた既存ログは今回のHive試験の記録ではなく、原因の実測根拠には使っていない。

## 自動応答のフック漏れを確認（NSL-route-v2、2026-10-30）

同じ1.26.52.3のローカルPEを再調査したところ、NSLの自動応答handler
`0x134AE30` がsenderのvtable[4]（sendToServer）を直接呼ぶことを確認した。
旧版のvtable[2]（general/chat send）だけのフックにはこの入口がなく、
Sync Repliesで本来保留したかった自動応答を捕捉できない構成だった。
slot 4へのフックとoriginal replayを追加し、両入口の到達・検証失敗・lock待ち失敗を
診断に追加した。ABI照合の詳細はSDK検証記録を参照。

今回指定されたログについて、こちらから読み出せた内容は10月29日21:59:49までの
非ゲームhostでのoverlayテストだった。今回のHive接続やVelocity Delayの行は含まれない。
CodexのLocalCache側でも同じファイルが確認されたが、保存領域のredirectが原因で
別のログを見ているのかまでは確定できていない。したがって、上記はPEと実装から
確認したフック漏れであり、今回のHive切断の実測原因を確定したものではない。

友人のMistは非公開の自作クライアントとのこと。内部方式は不明であり、今回の方式と
同じ、または低層ping操作を使うなどとは推定しない。KBの値を後から書く方式と
native packet/tick処理を遅らせる方式の差、他の確認・同期との整合性は引き続き未検証。

## 保留中の切断解消・KB解放時の切断報告（release-order-v3）

slot 4修正後、ユーザーから「保留中はerrorがなくなったが、KB発動時にerror」との
報告があった。これだけではNSLがHiveの唯一の確認機構、または唯一の切断原因とは確定しない。
旧試作の「適用後さらに1tick待って確認」を撤去し、適用成功と同じ更新内で対応する
確認を返す方式へ変更した。確認が移動の後追いになる可能性への対処である。
BoarではVelocityAck受理で次の予測用velocityを確定することを改めて確認したが、
これはFlareonの公開仕様ではない。[BoarDefaultAcknowledgments](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/ack/BoarDefaultAcknowledgments.java)

native SetActorMotionの予測・replay経路は現在の速度コピーに含まれないことも確認した。
この経路の利用状況を見分けるため、受信input tickとKB解放イベントをログへ追加した。
既存パスから読めたログは依然10月29日分であり、今回もHiveの切断直前を実測できていない。
次の実機記録を読み出せるよう、DLLの隣にもVelocity専用ログを出す。
8秒遅延がHiveで受理されるか、今回の変更で解放時の切断が解消するかは未確認。

## 実機ログ受領と予測キューへの変更（predicted-motion-v4）

`out/build/local/bin/velocity.log` を実際に読み取れた。2026-10-30 20:42:49–20:48:03、
v3、Delay=5,000ms。21件の受信KBは全件input_tick=0。write_fail、send_fail、
nsl_rejected、nsl_busyは全て0で、KB解放と対応する応答送信のSDK呼び出しは成功していた。
ユーザー報告は解放時のlagbackとkick。v3は補正/切断packetを観測していないので、
このログから切断の正確な時刻・理由や、サーバー側の応答受理を断定しない。

ローカルPEのSetActorMotionにある予測分岐を追い、native controllerが受信motionと
受信時刻をコピーしてイベントを所有することを確認した。v3の直接velocity更新は
この処理を通していなかった。v4はnativeのcomponent/feature gateが有効なとき、
同じenqueue/update関数を使う。元の受信時刻に実際の保留時間を加え、過去の時刻の
まま予測履歴へ入れることを避ける。通常のnative条件でpredictionが無効なら直接更新する。
詳細なRVA、ABI、境界検証はSDK検証記録を参照。

v4のログにはnative予測キュー/直接更新の別、予測補正packetと切断packetの到着を記録する。
これもサーバー側の全同期状態を遅延する方式ではなく、現行Hiveで5秒/8秒遅延が受理される
という実証ではない。受領したv3ログは `out/diagnostics/velocity-v3-hive-2026-10-30.log` に保存した。

## 最初の解放でのlagbackを再調査（release-trace-v5）

追加で受領したログは305行、57,590 bytes。v4 sessionは21:07:54.371開始、
21:11:03.979にDisconnect受信。再添付時も同じ内容であり、二つの別試験としては数えていない。
原本全体を `out/diagnostics/velocity-v4-hive-2026-10-30.log` に保存した。
このsessionの設定は5,000msで、8,000msの実測ではない。

### 確認できたこと

| 観測 | 判断できること | まだ判断できないこと |
| --- | --- | --- |
| 解放9件すべてでnative_predicted=0、directが1から9に増加 | 前回追加した予測キューの分岐は、この試験では使われていない | component不在、bypass、feature gate OFFのどれだったか |
| write_fail/send_fail/nsl_rejected/nsl_busyが全て0 | 観測対象の書き込み・送信呼び出し、コピー検証、NSLフックのlock取得は成功 | サーバー側での受理、送信の正確なwire順序、他の経路の漏れ |
| 通常KBの解放後139msと146msでゼロmotionを受信 | 解放に近接して別の速度通知が到着した | その通知が補正、通常処理、teleportのどれを意味するか |
| 大きな位置変化の後にも、以前の保留KBを解放 | 同じactor/worldの位置変更では古いキューを失効させない実装上の制約がある | 位置変更がlagback、死亡・respawn、試合移動のどれだったか |
| CorrectPlayerMovePredictionの記録なし、Disconnectあり | v4で観測したID161の補正経路は記録されなかった | MovePlayer等による別の補正の有無、サーバー内部の切断理由 |

最初のburstでは21:09:35.331に通常KBを解放し、35.470にゼロmotionを保留した。
35.933の解放時の位置は `(58.0403,79.8468,1.5541)`、36.436には
`(-0.1622,173.8231,0.5748)` だったが、その間に受けた位置変更packetは未記録。
続く37.032にも、元のburstのKBを適用している。
36.124/36.125には対象外motionの通過ログもあり、古い保留KBと通常のnative処理が
混在した。v4の通過ログにはtick・motionの実値がなく、これも補正と断定できない。

二つ目のburstでは21:11:01.374にKBを解放し、01.520にゼロmotionを受信した。
02.278の次の解放時、位置は最初の解放前とほぼ同じ場所に戻っていた。
その後も02.924、03.374と古いKBを適用し、03.979にDisconnectを受信した。
ユーザーのlagback報告と整合するが、位置の抜粋だけで補正packetの種類は特定しない。

### native受信処理との照合

同じローカルPEのSetActorMotion handler `0x13417D0` と、通常の直接更新側の
virtual slot `+0xB0` を再確認した。対象setter `0x1A0B8C0` はActorのcached
StateVector pointer `+0x218` を取り、velocity `+0x18/+0x20` の12 bytesをコピーして戻る。
直接更新側のsetter自体には、別の予測イベントを必ず発火する処理はなかった。
Mediconも同じStateVectorを引けていれば、この書き込み内容は一致する。
ただしv4ではcached pointerとの一致までは実測していない。

したがって「setterの代わりに生の値を書いたから必ず失敗する」、あるいは
「nativeの予測キューを強制すれば直る」という説明は、現時点では支持できない。
Bedrockのnative予測componentの利用条件と、Flareonがサーバー側で予測することも別である。
packetのhandled状態や処理時点まで、setterの同等性だけで証明されるわけではない。

公式説明では入力、予測位置、tickの報告はsimulation後に行われる。
Mediconの解放はLocalPlayerのbaseTickの元関数が戻った後であり、これだけで
「次の移動報告との正しい境界にいる」とは言えない。従来のログではNSL送信と
PlayerAuthInputのtick/位置/deltaの前後関係を記録していなかったため、
確認が進んだ最初の入力でサーバーが何を見たかを再構成できない。
これは最初のlagbackの原因を判断するうえで重要な未観測部分である。
[Mojang Player movement overview](https://mojang.github.io/bedrock-protocol-docs/guides/player-movement-overview/)

公開実装の追加確認では、RakNetのconnected ping/pongは独自の送信時刻からRTT等を計算し、
NSLとは別の通信層だった。ただし今回のHive接続でどのtransportが使われ、Flareonが
何を参照したかは観測していない。NSLだけの保留を自然な回線遅延と同一視しない。
[RakNet RakPeer.cpp](https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp)

MCBEの公開クライアントTorioにもDelayedVelocityの説明はあったが、確認した公開tree
`5cfff686f7dd7c1f4415fcf2e2e5d72cee9dcf17` には実装ソースがなく、READMEもsource未公開と説明する。
これをMistの内部方式や、現行Hiveで8秒遅延が動作する証拠には使わない。
[Torio-Client](https://github.com/Uncle-Awrt/Torio-Client)

### v5で追加した観測と次に判別する点

v5は解放アルゴリズムや確認応答の待ち時間を変更せず、以下をコピーして記録する診断版。
packetを追加で保留・書き換え・再送する変更は加えていない。

- `apply_begin/apply_end`: capture番号、実際の位置・速度、cached StateVectorとの一致、直接更新を選んだ理由。
- `nsl_hold/nsl_passthrough/nsl_submit`: 確認ID、保留・通過・再送時の観測。
- `pai_submit`: PlayerAuthInputのtick、position、delta、入力flags。
- `MovePlayer`: 自分宛てのmode、position、on_ground、到着時の古いKB/確認の残件数。
- 対象外motionの通過時にもtick、native受信時刻、motion、保留件数を記録。

移動・確認の詳細traceは原則として予定解放の250ms前から1,500ms後まで。
連続KBでは窓を結合し、実際の解放でも延長する。1窓2,048件で上限を明記する。
MovePlayerはDelay稼働中の別観測で、元のhandlerは常に呼ぶ。
描画lockがbusyなら待機せず観測を省略し、`move_observer_busy` を増やす。
このカウンターは他プレイヤーのMovePlayerも含むため、local補正の見逃し件数とは等しくない。
従って、ログに補正がないことだけで補正なしと判断しない。

共通seqはフック内での観測番号、threadは呼び出したthreadであり、
network batching後のwire順序やサーバー側の受理順序の証明ではない。
ログ自体のI/O負荷もあるため、診断版の計測値にはその影響が含まれる。
`Delay=0` は元の受信処理を通す比較用ログも出せる。

まず判別するのは、最初のKBを適用して確認を返した直後に、
KB前の位置/deltaを持つ移動報告が出ていないか、KBが最初の移動報告で反映されているか、
どの位置変更packetがいつ届き、古いキューがその後も生きているかである。
この観測前に再び1tick待ちへ戻す、予測分岐を強制する、ゼロmotionを一律破棄する等は
根拠を欠くため行っていない。Hiveでのlagback/kick解消はまだ実証されていない。

v5のRelease成果物は `out/build/local/bin/Medicon.dll`（39,483,904 bytes、
2026-10-30 21:38:18更新）。DLL内の `release-trace-v5` markerを確認した。
core/commands/gameplay_modules/bedrock_chat/bedrock_gameplay/gui/runtime/
gameplay_visuals/preview_smoke/dll_lifecycleの10件が成功。
最後の送信経路別のtrace追加後にもbedrock_gameplay/runtime/dll_lifecycleの3件を再実行し成功した。
追加したpacket読取テストとローカルPEのprefix/vtable照合も成功。
Minecraftへの注入・Hive上での解放試験は、この調査作業からは行っていない。

## v5実機結果と補正後の古いKBの失効（position-reset-v6）

画像で再びHiveからの切断が報告された。新しいsidecarは947行、202,885 bytesで、
v5 sessionは21:40:12.559に開始し、21:42:45.316にDisconnectを受信した。
原本は `out/diagnostics/velocity-v5-hive-2026-10-30-214245.log` に保存した。
画像にある不透明なError文字列から、管理側の検知名・判定理由は読み取れない。

この試験では全6件の適用が `direct:no-component`、全件 `cache_match=1` だった。
したがって今回の直接更新は通常setterが参照するのと同じStateVectorに届いており、
予測component不在が直接更新を選んだ理由である。書き込み・送信・NSL検証・NSL lock待ち・
MovePlayer観測のbusyは記録上すべて0だった。general/sendToServerのPAIは同一thread・
pointer・tick・内容の対になっており、二重送信としては数えない。

### 切断前の確定した順序

| 時刻（JST） | 観測 |
| --- | --- |
| 21:42:42.924 | tick2652、位置 `(-16.5766,39.6200,-42.6640)`、KB適用前の移動報告 |
| 21:42:42.975 | 同位置のStateVectorにKB `(-0.1346,0.4000,0.3767)` を適用し、確認4件を投入 |
| 21:42:42.976 | tick2653、位置 `(-16.7000,40.0200,-42.2874)`。Yが0.4進み、KBは最初の移動報告へ反映済み |
| 21:42:43.106 | ゼロmotion受信後、MovePlayer mode2、位置 `(-16.5766,39.6700,-42.6640)` を受信。解放から131ms、旧KB4件が残存 |
| 21:42:43.121 | tick2656が補正先の位置、delta0、HandledTeleport bit37付きで送信される |
| 21:42:45.120 | 補正前に受けた次のKB `(0.4000,0.4000,0)` を再適用し、確認2件を投入 |
| 21:42:45.316 | その196ms後にDisconnectを受信 |

これで、このburstの位置戻りが単なる見た目の揺れではなく、自分宛てのteleport-mode
MovePlayerによるサーバーからの位置変更だったことが確認できた。
KBがStateVectorに届いていない、あるいは解放直後の最初のPAIがKB前のまま、という
説明はこの記録とは合わない。ただしAPI投入順序からnetwork wire順序は断定しない。
比較的前の21:42:19.466、20.024、20.621の解放後には、このログでMovePlayer補正は
記録されていない。5秒遅延が常に同じ結果だったとは扱わない。

確実に直せる不具合は、同じactor/worldのteleportを古い保留KBの失効条件として
いなかった点である。今回もサーバーの位置変更を受けてnativeが移動状態を更新した後に、
補正前のKBを後から上書きしていた。これが後の切断を起こしたとまでは、時間的な近さ
だけでは証明できない。また、この不具合は最初の位置補正を説明するものでもない。

### v6の変更

自分宛てのMovePlayer mode1/2（Respawn/Teleport）を受けたら、保留KBを失効させる。
確認応答は破棄せず、失効したsequenceまでを次の有効なgame tickからFIFOで返す。
補正後に受けた新しいKBは新しいsequenceで保留し、その確認を早期解放しない。
mode0/3、未知mode、他プレイヤーの通知ではlocal KBを失効させない。
元のMovePlayer handlerとnativeのteleport確認は通常どおり動かす。

packetの型・layoutをimmutable profileで照合してscalar通知だけをコピーし、
その場の描画lockがbusyでも通知を保持する。lock取得後または次のgame tickで、
現在のlocal runtime IDと照合してから失効を適用する。後続のKB/NSLを受け付ける前にも
保留通知を処理する。通知キューは256件までで、overflowは診断に残す。
native packet、shared_ptr、actor pointerをキューの所有対象にはしていない。

回帰テストは「最初のKB解放→同じactor/worldの補正→古いKBの期限→新しいKBの期限」を
再現し、古い2件の不適用、確認の保持・FIFO・失敗再試行、新しいKB/確認の独立した期限、
respawnとOFF時のcleanup、通常/head-only/未知modeの非介入を確認する。

### 最初の位置補正について残る点

Flareon内部の検知条件は確認できていない。PAIが次のsimulationを反映していることから、
根拠なく「確認をさらに1tick待つ」等へ戻してはいない。
また、公開Boarでは同じ確認処理がKBだけでなくブロック、chunk、attributes、effects、
teleportの状態も進める。確認だけを遅らせて他の受信を直ちに反映すると、別の状態まで
サーバー側の予測とずれる可能性がある。これは公開例からの推論であり、今回のHiveで
そのどれが検知されたかの実証ではない。任意の追加packetを止めれば直る、とは判断しない。
[BoarDefaultAcknowledgments](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/ack/BoarDefaultAcknowledgments.java)、
[LatencyUtil](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/util/LatencyUtil.java)

v6は補正後の古いKBの再適用を直す版であり、初回lagbackやHiveの切断を解消したという
実機結果はまだない。試験条件の5秒、要望の8秒のどちらも受理を保証する結果ではない。

v6のRelease `out/build/local/bin/Medicon.dll` は2026-10-30 21:54:42更新、
39,486,464 bytes。DLL内の `position-reset-v6` markerを確認した。
関連する10テスト（commands/core/gameplay_modules/bedrock_chat/bedrock_gameplay/
preview_smoke/runtime/gameplay_visuals/dll_lifecycle/gui）は全件成功した。
SHA256: `5586B19474E837D7FAC7C11FE90A666E43CB39C63543FC9FD70DE99C340760F4`。

## prediction AC 下の解放拒否と零motion（zero-stop-v7、2026-10-30）

`out/build/local/bin/velocity.log` の 22:53:33–22:57:44（v6、Delay=5,000 ms）を
再解析した。この試験は8,000 msではなく5,000 msであり、以下の結論を8秒の実測として
扱わない。前回保存した v3/v4/v5 のログとも突き合わせた。

### 確認できたこと

| 時刻（JST） | 解放前の保留 | 解放時に保留していた確認 | 解放後のサーバー応答 |
| --- | --- | --- | --- |
| 22:55:13.393 | 新しいKB 3件 | 5件 | 131 ms後に零motion＋MovePlayer mode2で解放前位置へ |
| 22:55:30.798 | 新しいKB 0件 | 44件を同時送信 | 補正なし |
| 22:55:38.544 | 新しいKB 3件 | 25件 | 106 ms後に零motion＋MovePlayer mode2 |
| 22:56:07.142 | 新しいKB 0件 | 0件 | 直後の補正なし（5.7秒後の別resetまでなし） |

解放そのものは成功している。22:55:13.393の適用直後、13.394のPAI tick 1604は
delta `(-0.2076, 0.3136, -0.0679)` を持ち、適用したKB
`(-0.3802, 0.4000, -0.1243)` が最初の移動報告へ反映されている。その約130 ms後に
零motionと、解放前の `(-5.3007, 42.6700, -68.8822)` へのteleportが届く。
これは確認応答の順序だけでは説明できず、サーバーがその時点の速度・位置変化を
拒否したと読むのが自然である。ただしFlareonの内部判定名・閾値は取得できていない。

v5の21:42:42.975でも、適用→次tickのPAI反映→131 ms後の零motion＋mode2という
同じ順序だった。v3/v4で記録した切断も解放の直後に起きている。遅延中（保留中）には
同じ補正が起きていないため、「KBを適用していないこと」自体ではなく、
「元のtickを過ぎた後に説明のない加速を適用したこと」が直接の引き金である可能性が高い。

### Sync Repliesとの相関は否定された

22:55:30.798は44件の保留確認をKBと同時に送信したが補正されず、
22:55:13.393は5件の送信で補正された。保留確認の件数や一斉送信だけでは
補正の有無を説明できない。同期応答の保留が無関係と確定したわけではないが、
少なくとも今回の最初のlagbackの主因を「確認がKBを追い越したこと」に置く根拠はない。
5–8秒のNSL保留は別のtimeoutを招く可能性があるため、切り分け試験では
Sync Replies OFFも比較する必要がある。

### 零motionを遅延キューへ入れてはいけない

サーバーはKBだけでなく、速度を止めるための零motionも送る。v6はこれを通常の
KBと同じく保留していた（22:55:13.524に `motion=(0,0,0)` を `kb_pending=4` として記録）。
今回のburstでは直後のMovePlayer resetが古いキューを捨てたため実害が出なかったが、
零motion単独の受信（22:00:59.193、22:03:27.533、22:07:11.324、22:44:35.720 など）では
その停止を5秒後まで適用しない構成だった。停止を遅らせると、サーバーが速度0を
前提にした後にクライアントだけが動き続けるため、別の位置補正を招き得る。

v7は零motion（各成分 `<= 1e-6`）を即時にnativeへ通し、古い保留KBを失効させる。
零motionはKBとして数えず、`zero_motion` 診断だけを増やす。これは確認応答の順序や
遅延時間を変える修正ではなく、零motionをKBとして扱っていた明確な不具合の修正である。

### 8秒が通らない理由の切り分け

現時点の実装は、サーバーが元tickで処理したKBをクライアント側だけで後から
StateVectorへ書く。サーバーは保留中に「KBを適用していない」ことを連続的には
咎めていないが、後から現れた加速は拒否する。したがって、遅延時間を伸ばすほど
元tickとの差が広がり、拒否される確率は上がる。5,000 msでも解放の一部が拒否されており、
8,000 msの受理を示す実測はまだない。Mojangの例示するrewind履歴40 tick（2秒）を
超える遅延を、サーバー側の予測と一致させるには、速度だけではなく入力・位置報告の
タイムライン全体を遅らせるか、元のinput tickへ正しく戻す必要がある。どちらも
現在の実装には含まれず、Hiveで有効という証拠もない。

現実的な次の切り分けは、`Delay=0` を基準に 250 / 500 / 1000 / 2000 ms を
Sync Replies ON/OFFで比較し、`zero_motion` と解放後300 ms以内のMovePlayer mode1/2を
数えることである。短い遅延でも解放直後に補正されるなら、原因は遅延長ではなく
「後から速度を書くこと」自体にある。補正が遅延長に依存するなら、その閾値以下のみを
許可するのが安全である。連続被弾では、古いKBをFIFOで1件ずつ解放する現方式が
サーバーの最新KB状態とずれる可能性があるため、次段では「最新1件へ集約」または
「解放後300 ms以内の補正を検出したら一定時間解放を停止」を比較する価値がある。
どちらもまだ実装・実機確認していない。

### 内部ping spoof（internal-ping-v10、2026-10-31）

UIの`Ping Spoof`欄は削除し、Delay中は**Delayと同じms**を内部値として
NetworkStackLatency応答の`mCreateTime`から引く。Delay=5,000なら5,000 ms。
Sync Replies ONなら書き換えたコピーをKBと一緒に保留し、OFFなら即時送信する。
RakNet ConnectedPingのフックは実機で`calls=0`だったため再追加していない。

v10のReleaseは `out/build/local/bin/Medicon.dll`（39,495,680 bytes、
2026-10-31 00:34:52、SHA256
`5BAE77F44FA9C80E201068746ECB09131C673A4535208FA2E1CD16777C7B8EFC`、
`internal-ping-v10` marker確認済み）。関連4テスト
（gameplay_modules/runtime/core/bedrock_gameplay）とDLL lifecycleは成功した。
零motionの即時処理はv7から維持している。`mCreateTime`がnonceなら切断する
可能性があるため、`nsl_spoofed`が増えてkickする場合は内部spoofを無効化する。
Hiveへの再接続・注入・v10の実機試験はこの作業では行っていない。

## 試合開始付近の切断と応答値の書き換え撤去（opaque-nsl-v11、2026-10-31）

「待機時間が終わって自由に動ける瞬間にkick」という報告を受け、現行ソースと
`out/build/local/bin/velocity.log` を再確認した。調査時のログは1,321,962 bytes、
最終行は2026-10-31 00:45:54.043。全体のコピーを
`out/diagnostics/velocity-v10-hive-2026-10-31.log` に保存した。

### 今回のログで確認できたこと

| session / 時刻（ログ表記、JST） | 観測 |
| --- | --- |
| v10、00:36:20.978開始 | 00:40:09.338にMovePlayer mode2。その後NSL spoofの計数が2へ増加、00:40:20.154にDisconnect |
| v10、00:42:26.926開始 | 00:45:43.934にMovePlayer mode2。その後NSL spoofの計数が2へ増加、00:45:54.043にDisconnect |
| 上記2回とも | 切断直前まで `kb_queued=0 kb_applied=0 held=0 sent=0 nsl_spoofed=2` |

したがって、今回の2回は遅延KBの解放時の切断ではない。KBを一度も保留・適用せず、
NSLへのspoof処理を通り、native送信へ戻した後に切断されている。`sent=0`は
保留応答の再送が0という意味で、native経路の送信が0という意味ではない。
また、v10の`nsl_spoofed`はコピーの減算成功を数え、元packetへの書き込み結果や
wire上の到達・受理までは記録しない。両send hookを通る場合の二重計数もあり得るため、
2つの異なるpacketを送ったとまでは断定しない。MovePlayerが試合開始に
対応するかはpacketログ単独では確定できないが、ユーザー報告とは矛盾しない。

### コード上の問題

v10はDelayがONなら、KBの有無やSync RepliesのON/OFFにかかわらず、
送信NSLの `+0x30`（mCreateTime）からDelay相当の値を引いていた。
値の大きさからns/µs/msを推定しており、サーバーが実際に時計として扱うのか、
どの単位なのかを確認していなかった。
前記v10の記録にはslot 4のみでspoofするとあるが、今回確認した実際のソースは
general send / sendToServerの両方から同じspoof処理を呼び、二重減算を防ぐgateもない。
v11では両方から書き換えを撤去した。

NSLの値はクライアントが自由に作り直せるローカル時計とは限らない。
公開例Boarは、送受信のtimestampを確認transportに渡し、server-stateの確認に使う。
これはFlareonの内部実装の証明ではないが、timestampという名前を根拠に減算してよい
とは言えないことを示す。IDとして照合するサーバーなら、値を変えた応答は別のIDになる。
[Boar NetworkLatencyPackets](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/packets/other/NetworkLatencyPackets.java)、
[BoarAcknowledgmentTransport](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/ack/BoarAcknowledgmentTransport.java)

今回の最有力はこの書き換えによる応答の不整合。切断理由payloadやサーバーログは
取得しておらず、値の不一致を即時検出したのか、確認が進まずtimeoutしたのか、
別の理由かまでは確定しない。過去v3–v7の「KB解放後の位置補正」とは区別する。

### 修正

- 内部spoof、SDKの書き換えAPI、単位推定helperを撤去した。
- NSLは値を変えずに通過させる。Sync Repliesで保留するコピーも全bitを維持する。
- KB未受信の状態では返信を保留・生成しない。
- Sync Replies OFFへの切替時にも、未送信FIFOを新しい応答が追い越さないようにした。
- session markerは `opaque-nsl-v11`。KBの遅延キュー、零motionの即時処理は維持した。

これは不正な応答値を作る処理の修正である。5秒/8秒後のKBがサーバーに受理される
ことを証明する変更ではなく、v11の実サーバー試験もまだ行っていない。

### ビルド・検証

Releaseのソースコンパイルは成功。通常名の`Medicon.dll`は書き込みを拒否されたため、
同じ生成済みlink ruleとobject/libraryで別名のDLLへリンクした。
成果物は `out/build/local/bin/Medicon-opaque-nsl-v11.dll`、39,494,144 bytes。
SHA256: `2C34A2A841A9A3743B10CC60BEB085C466E12FD87CF8E89ED542C4ED47CDB453`。
新しいDLLの`opaque-nsl-v11` markerと、旧`internal-ping-v10` / `nsl_spoof`文字列の
不在を確認した。通常名のDLLは旧版のままなので、修正の確認には別名の成果物を使う。

CTestのcore / gameplay_modules / bedrock_gameplay / runtimeは4件成功。
別名の新DLLを明示して実行したexport/lifecycleテストも成功した。
opaque IDの全bit保持、KB未受信時の非保留、保留・失敗再試行後の同一性、
Sync Replies OFFへの遷移後もFIFOを維持することを確認している。

## 次に検証する遅延方式の選定（2026-10-31）

ユーザーの「ping spoofは消して、実際にできそうなbypassを考えて」という指示に
対する検討。新しいbypassが成立したという記録ではない。この検討で追加のspoofや
新モードは実装していない。現行src/include/testsにはNSL/RakNetのping spoof実装はない。

再確認時、通常名の `out/build/local/bin/Medicon.dll` にも `opaque-nsl-v11` markerが
あり、`internal-ping-v10` markerはなかった。39,494,144 bytes、SHA256:
`1E86FD3CB04B7B02A370E0C71B8BF32E2E8DE3FD92FCF73D02BD5A1A0EBFC992`。
前節の通常名が旧版という記述は前回ビルド時点の状態であり、現在の状態とは異なる。

### 実装されていたものと、実証されていない効果

以前のRakNet ConnectedPing案は実機hook到達0で、機能した証拠がなかった。
v10はNSLの応答値を減算するコードを持ち、実機でその計数も増えた。
しかし、それがサーバーの測定pingを増やすか、KBの受理条件を変えるかは確認せずに
導入されていた。コードの存在を、動作するping spoof/bypassの実装と扱うべきではない。

### 第一候補: 受信ゲーム状態のFIFO遅延とnative処理の再開

根拠は、KBだけでなく属性・効果・ブロック・teleport等も同じ確認機構で進める
公開実装があること。KBとNSLだけ止めて他を直ちに処理すると、クライアントと
サーバーの予測が別の状態集合を参照する可能性がある。
[BoarDefaultAcknowledgments](https://github.com/opencollab-incubator/Boar/blob/8ff1e171826d066fd9d24940e777a5b6f7869172/common/src/main/java/ac/boar/anticheat/ack/BoarDefaultAcknowledgments.java)

具体的な設計候補は次のとおり。

1. 接続維持のtransport層と分離できる、受信ゲームpacketの所有権が明確な入口を使う。
   現行dispatcherの借用ポインターを保存して後から呼ぶ実装にはしない。
2. KB単独ではなく、同じ受信ゲーム状態ストリームを順序を保って保留する。
   先行するpacketを後続の確認が追い越さない。受信時の間隔も記録する。
3. 解放時にoriginal native handlerへ戻し、simulation前の通常の処理境界で消費させる。
   現行の「速度だけを後からStateVectorへ書く」経路を使わない。
4. NSLは通常処理から元のIDで返す。PAIのtick/位置を架空の値へ書き換えない。
   複数packetが同一simulationに入る場合、その境界も観測して保持する。

これは整合した実遅延を再現する設計仮説。Flareonがその遅延を認める証拠はない。
敵・ブロック等の表示更新も遅れ、リアルタイムの戦闘感覚は維持できない。
接続維持やsession変更も含めた所有権・期限管理が必要で、既存KBキューの小修正では
実現しない。8秒をそのまま許容させる方法とは扱わない。

### 最初に区別すべき試験

同じ条件で、native無変更を基準に、packet内容を変えない実遅延と現行の選択的遅延を
比較する。0 / 50 / 100 / 250 msは試験点であり、許容閾値や成功設定ではない。

- 実遅延は通り、選択的遅延だけ解放後に補正されるなら、処理順序・状態の不整合を追う。
- 両方で補正されるなら、確認packetの追加操作で解決すると決めつけない。
- 短い遅延が通っても、5秒/8秒や継続利用まで外挿しない。

観測はKB受信、native適用、NSLの元ID/返送、最初に影響したPAI tick、補正・切断を
同じ時系列で対応させる。切断理由を未取得のまま、全てをvelocity checkとは呼ばない。

なお、同じ確認に束ねられた複数KBを現行の「1tick1件」で人工的に分割する点も
検証対象になる。ただし保存ログ39回のKB解放は全て確認の再送計数も増えており、
「確認を返さずにKBだけ適用した」事例はこのログからは見つからなかった。
これを今回の原因や、最新KBへ集約すれば通る根拠としては使わない。

## Stream Delayの試作（receive-stream-v12、却下・撤去済み、2026-10-31）

**ユーザーの「この仕組みだめだわ却下」により撤去した。** 以下は試作時の記録であり、
現行実装・推奨方式ではない。Stream Delayの設定、受信pump/helper hook、所有buffer/FIFO、
専用テスト・ビルド登録を削除し、追加前のFull/Percent/Delayへ戻した。
NSL timestampの書き換えは復活させず、元の確認値を返すv11の修正を維持する。
撤去後の通常名DLLを再ビルドし、`opaque-nsl-v11` marker、Stream Delay/受信hook/旧spoof
文字列の不在を確認した。関連6テスト（core/gameplay_modules/bedrock_gameplay/runtime/
dll_lifecycle/gui）は全件成功。撤去後DLLは39,494,144 bytes、SHA256:
`069A00F25E735EB666476EBC885939E8297371543408B98E57157C755C766FD3`。

上記候補を実装する指示を受け、Velocityに`Stream Delay`（choice index 3）を追加した。
従来のFull/Percent/Delayは比較用に残し、既存設定のindexを変更しない。
新方式の操作は`Mode=Stream Delay`と`Delay (ms)`。Sync Repliesは新方式では非表示。

PEの受信pump `0x16AADF0` と受信helper `0xD935A0` を追い、借用Packet参照を
後から呼ぶ必要のない入口を確認した。helperはnative stringにデコード前のgame payloadと
元の受信時刻を返す。新方式はstringの32-byte headerを所有権ごと移動し、期限後の同じ
helper呼び出しで元bufferを返す。元のnative decoder/dispatcherが処理を再開するため、
KBのStateVector直接更新を使わず、NSLの応答も元のnative handlerが生成する。
packet内容・input tick・受信時刻・NSL応答IDは書き換えない。

実装詳細とRVA/命令の照合は [SDK検証記録](gameplay-sdk-validation.md) を参照。
今回使う全6組の命令prefixは、同じローカル1.26.52.3 PEと一致した。
クライアントcallbackを持つpumpにthread-local scopeを限定し、同じhelperを使う
サーバー処理を保留しない。キューにconnection/callback/shared_ptrは保存しない。

### 所有権・順序・ライフサイクル

- 非blockingな元のreceiveをpollし、各payloadを取得した時刻+設定msの期限を付ける。
  物理wire到着時刻の再現を保証するものではなく、native helperで観測した順序と
  期限を保つ。元の受信時刻metadataは内容を変えず保持する。
- 期限後は新しい受信より先に古いbufferを返す。複数期限が同時に来た場合は
  native event pumpの通常のbudget/バッチで処理し、人工的に1KB/tickへ分割しない。
- 最大4,096 buffer / 64 MiB。閾値へ達した、または1bufferで超えた場合は、
  追加受信の前にFIFOを早期解放する。`overflow`に記録し、遅延時間の保証を短縮する。
- OFF・mode変更・UI入力・focus喪失では、次のnative receiveから順序どおり解放する。
  死亡・world/actor/connection identity変更では古い受信bufferを破棄する。
- 入口でDisconnect/StartGame/Respawn/ChangeDimensionを見た場合は、古いbufferを破棄し
  当該packetをnativeへ通す。新しいsnapshotが公開されるまで再保留を止める。
- retained allocationは、通常の交換・破棄・eject時ともゲーム側deallocatorへ返す。
  DLL allocatorで解放しない。hook/profile不成立時に旧Delayへfallbackしない。

`Velocity Stream:`はpending/bytes、seen/queued/released/discarded、boundary、overflow、
layout rejectionを記録する。nativeへ戻ったmotionは`Velocity Stream native motion:`、
その後の移動/NSLは既存のbounded traceでも観測できる。hook/register・wire送達・
サーバー受理は別の観測であり、カウンターの増加だけでbypass成功とは判断しない。

### ビルドと検証

Release DLL: `out/build/local/bin/Medicon.dll`、39,509,504 bytes。
SHA256: `564CFAD77A7B9446D21A251CA261EA535224E544AD8B102FB23514F46821A12E`。
`receive-stream-v12` / `Stream Delay` markerを確認し、旧spoof文字列の不在を確認した。

関連7テスト（receive_stream / core / gameplay_modules / bedrock_gameplay / runtime /
gui / dll_lifecycle）は全件成功。新しい受信テストは期限・設定短縮・count/byte上限・
move-only所有権、native stringのinline/heap/4KiB alignment、元NSL bytes/時刻の全bit保持、
OFF時FIFO、client scope、connection/world変更・明示clearでの解放、0ms通過を検証する。
空キューへOFFのdrainを要求した後でも、次のONで最初のpacketが期限前に通らないことも
回帰テストに含める。既存の初期化不足/deprecated filesystemの警告は本変更と無関係。

実ゲームへの注入・サーバー接続・0/50/100/250ms比較はまだ行っていない。
敵やworld更新も遅延する設計どおりの実装であり、8秒の受理やFlareon bypassの成立を
確認したものではない。

## Delay2 モード（削除、2026-10-30）

RakNet ConnectedPing の timestamp を書き換える Delay2 を試験実装したが、実機ログで
`SendImmediate` フックにID 0のパケットが一度も届かず（ping_seen=0）、効果を確認できないまま
削除した。参考にした公開実装は Solstice の Flareon Disabler（`1d7808f`）。
