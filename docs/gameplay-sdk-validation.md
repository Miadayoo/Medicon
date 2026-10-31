# Gameplay SDK ABI 検証

調査日: 2026-10-27（日本時間）。実ゲームに接続した動作確認ではなく、ローカル PE の限定的な照合と fixture テストの記録。

参照元は [surukotonai/solstice](https://github.com/surukotonai/solstice/tree/1d7808f52ef4ba6e34a7fbcc65294907a7761611)、commit `1d7808f52ef4ba6e34a7fbcc65294907a7761611`。`OffsetProvider.hpp`、`EnttHashs.hpp`、Actor components、`GameMode.hpp`、`BlockUtils.cpp` をレイアウトと呼び出し方法の参考にした。上流 SDK、EnTT、ゲームの STL オブジェクトをこの DLL に取り込んでいない。

対象実行ファイルは既存の [Main Menu 検証記録](main-menu-sdk-validation.md) と同一。

| 項目 | 値 |
| --- | --- |
| ファイル | `C:\Users\hina\AppData\Roaming\levilauncher.exe\versions\1.26.52.03\Minecraft.Windows.exe` |
| FileVersion | `1.26.52.3` |
| SHA-256 | `47DE440DC290BB5BD2A4CCD79E39C00219A155EA2C23BB961E27E323BD2FD942` |
| PE TimeDateStamp | `0x6AB54E37` |
| SizeOfImage | `0x12C02000` |

新しい gameplay bridge は **FileVersion 1.26.52.3 のみ** を受理する。ハッシュは調査対象の記録であり、起動時の全ファイルハッシュではない。

## Velocity（2026-10-30）

同じローカル `1.26.52.3` PE で SetActorMotion（ID `0x28`）の factory、vtable、
dispatcher と serialization を照合した。Packet vtable は RVA `0xE8A73D0`、
getId は `0xB270F0`（`B8 28 00 00 00 C3`）。runtime ID は packet `+0x30`、
motion の X/Y/Z は `+0x38/+0x3C/+0x40`、tick は `+0x48`。
serialization の読み取りは `0x3DFF80C..0x3DFF890` で確認した。

dispatcher vtable は `0xE7BF8C0`、handle は `0x1733000`。
handle は R9 の借用 shared_ptr から Packet* を取り出し、R8 の callback の
virtual slot `+0x668` へ tail-call する。参照カウントの増減・破棄処理は持たない。
フックは4引数をそのまま保持し、ゲームの shared_ptr を DLL 内で構築・破棄しない。
バージョン、handle の先頭16 bytes、dispatcher vtable の関数ポインターを確認してから有効化する。

現在の local actor の registry/entity と ActorOwner を再検証し、
RuntimeIDComponent（hash `FC0DBBB5`、8 bytes）と packet の runtime ID が一致するときだけ処理する。
この component hash は `0x19E4A8F`、payload accessor は `0x227420` で照合した。
packet vtable、getId の命令、motion の有限値も検証する。
Full は dispatcher を呼ばずに戻り、既存の移動速度を変更しない。
Percent は X/Z と Y に独立した 0–100% を適用し、motion の12 bytesだけを書き換えて native dispatch に渡す。
入力を GUI が捕捉している間、ゲームの mouse grab がない間、フォーカスがない間は処理しない。

Release ビルドと core・gameplay_modules・bedrock_gameplay・gui・runtime・DLL lifecycle 等の
テストで設定保存、範囲検証、軸ごとの倍率、他 actor の ID 拒否、隣接フィールド保持を確認する。
実ゲーム内のノックバックとサーバー側の挙動は別途確認が必要。

### Delay の試作（2026-10-30）

Mode に Delay を追加した。`Delay (ms)` は0–10,000、初期値8,000、slider step 50。
受信時は検証済みの motion 3値だけをコピーして保留し、元packetの処理を止める。
借用 shared_ptr、dispatcher の network/callback 引数は保持しない。
待機は steady_clock の期限で管理し、ゲームや描画スレッドを Sleep させない。

期限後の local baseTick で、`GameplayAccess::set_velocity` から既存の native scope を検証する。
現在の client/player、thread、registry/entity、ActorOwner を照合し、36-byte
StateVectorComponent の velocity `+24` だけを更新する。position と previous position は保持する。
コピーした world/actor/unique ID が変わった場合や、死亡、入力捕捉、OFF、world clear では保留を破棄する。
受信と更新は既存の render_mutex で直列化する。失敗した書き込みは次のtickで再試行する。

各hitの期限は受信時の設定値で固定し、FIFO順序を保ち、1tickに最大1件を適用する。
キュー上限256件では新packetをそのまま通す。元packetのtickでのrewind、pingの書き換え、
確認応答の遅延は行わない。この試作は受信KBを後からローカルvelocityへ適用する機能であり、
現行Hive/Flareonの検知回避や8秒遅延のサーバー受理を確認したものではない。

### 位置戻し報告後の Sync Replies（2026-10-30）

上記の最初の試作に対し、保留中・適用時に位置戻し／キックされるとの報告があった。
通信の確認だけがKBより先に進む経路への対処として、DelayにSync Replies（初期値ON）を追加した。
対象は送信される NetworkStackLatency（ID `0x73`）で、Hiveの現行判定がこれを利用するという実測はない。

同じPEのfactoryで ID 115 → `0x16BAC90` を追い、packet vtable `0xE7B0FD0`、
getId `0x3C40700`（`B8 73 00 00 00 C3`）、dispatcher `0x1185B5B0` を照合した。
constructorのallocationは shared control block 16 bytes を含む `0x58` なので、
packet本体は `0x48` bytes。timestamp `+0x30`、fromServer `+0x38`、`+0x40` の追加値まで
含めてコピーする。上流の簡略定義で推定する `0x40` bytes には切り詰めない。
constructor `0x16BADC0..0x16BAE11` にあるpacketのフィールドはscalar値とstaticな
native vtable/dispatcherで、所有するheapフィールドはない。

送信hookは有効なDelayまたは応答の後処理中だけ、まずvtableで対象を絞る。
対象packetに限り既存render_mutex下で、現在のclientのsender、vtable/getIdの命令、
dispatcher、bool範囲、コピー長を検証する。借用packetのアドレスを保存せず値をコピーする。
応答には直前KBのsequenceを対応付け、適用成功と同じ更新内にFIFOで返す（下記v3で訂正）。
KB書き込み失敗時は確認を先に返さない。後続の応答も保留列を追い越させない。

再送時はbaseTickのmutation scope、現在のsenderと同一のnative identityを再検証し、
送信hook作成時に保存したoriginal trampolineへコピーを同期的に借用させる。
ゲームのshared_ptrは構築・破棄せず、保存済みsender/sessionを後からdereferenceしない。
OFFや入力捕捉ではゲームtickで応答の後処理を行い、別world/actor/snapshotには持ち越さない。

選択的な確認遅延は本当のネットワーク遅延と同一ではない。PlayerAuthInputやworld更新、
RakNetのping/pongは変更しない。同じ確認がKB以外の同期にも関係するサーバーがあり、
timeoutや現行Flareonの適合性は未確認。`Velocity Delay:` のカウンターで対象が観測されたか、
KBが適用されたか、応答が送られたかを切り分ける。

### 自動応答の送信経路の訂正（NSL-route-v2、2026-10-30）

再度のキック報告を受け、同じPEのNSL受信dispatcherから自動応答までを確認した。
dispatcher `0x1733CE0` は callback virtual slot `+0x2B0` へ渡す。
自動応答handler `0x134AE30` は `packet +0x38 == 1` の場合、NSLをstack上へ
コピーし、`0x134AE76..0x134AE89` で handler `+0x78` のsenderの
**virtual slot +0x20（vtable[4]、sendToServer）** を直接呼ぶ。
呼び出し後、元packet `+0x11` を1にする。コピーは同期呼び出し終了までの借用である。

旧試作がフックしていたのは general/chat send のvtable[2]だけだったため、
この直接送信経路を捕捉する入口がなかった。現行版は検証済みsenderからslot 4も取得し、
専用フックを追加する。両slotの関数アドレスが同じ場合は既存フックを共有する。
保留応答のreplayとsender再検証もslot 4へ揃えた。

起動ログに `NSL-route-v2` を記録する。Delayの定期ログにはnative対象のhook到達数
`nsl_hook_hits`、packet/senderの検証失敗数 `nsl_rejected`、4ms以内にlockを取れず
通過した数 `nsl_busy` も出す。通常send経由で通過した応答はslot 4へ転送される場合が
あり、hook到達数は固有packet数とは限らない。これらとmodule側の `held` / `sent` を
分けて確認する。この経路修正だけで今回のHiveキックの原因や8秒の受理が確定するわけではない。

### KB解放時の確認順序（release-order-v3、2026-10-30）

「保留中は切断されず、KB適用時に切断される」という報告を受けた。旧版の
適用後1更新を待つ条件を削除した。KBが反映された移動を確認より先に送る可能性が
あるため、成功した速度適用と対応する確認送信を同じ更新内で行う。書き込み失敗時は
そのKBの確認を返さず、後続KBの確認も先行させない。送信失敗時のFIFO再試行は保持する。
同期限の連続KB、適用→確認の呼び出し順、送信失敗後に新しい確認が追い越さないことを
テストする。これは確認順序を変える修正であり、Hiveの切断理由を実測確定したものではない。

再調査したcallback vtable `0xE7AF110` はslot `+0x2B0` がNSL `0x134AE30`、
slot `+0x668` がSetActorMotion `0x13417D0`。前回記録のNSL dispatcher RVA/slotは
隣のtableを誤って読んだため、上記の値へ訂正した。slot 4への自動応答送信という結果は同じ。
SetActorMotionの `0x13419D2..0x13419E5` はPredictedMovementComponent（hash
`18513471`）経由の処理を持ち、`0x1341ACD..0x1341B53` はReplayStateComponent
（hash `9FC78512`）と非zeroのpacket tickを使う経路を持つ。fallbackはActor virtual
slot `+0xB0` で、確認したsetter `0x1A0B8C0` はStateVectorのvelocityのみをコピーする。
v3のDelayはfallbackと同じ値の更新であり、上記の予測/replay経路を再現していなかった。
どの経路が当該Hive接続で使われたかは未確認。

KB受信時はpacket `+0x48` のinput tickもコピーしてログに出す（書き換えない）。
queue/releaseイベントにはミリ秒時刻を付け、適用と確認の差、書き込み/送信失敗、
位置・速度を記録する。MinecraftではDLL隣の `velocity.log` にVelocity行も保存し、
LocalAppDataのhost別redirectで診断ログを取り違える可能性を減らす。隣のdirectoryへ
書けない場合も起動を止めず、従来のinject.logを使う。

### 予測キューへKBを渡す（predicted-motion-v4、2026-10-30）

提供された実機の `velocity.log` は20:42:49–20:48:03のv3記録で、Delayは5,000ms。
全21件の受信input tickは0、SDKのwrite/send失敗、送信対象の検証失敗、lock待ち失敗は
いずれも0だった。20:48:02.277と20:48:03.481にも解放成功の記録がある。
これだけではHiveが応答を受理したか、どの判定で切断したかまでは分からない。
非zero input tickによる過去のreplayだけを原因とする説明はこの記録では支持されない。

SetActorMotionの予測分岐を追加で逆アセンブルした。componentの要素幅は `0x90`、
先頭byteが0かつ引数なしのfeature gate `0x1040080` がtrueの場合、`component +0x30`
のcontrollerへ `0x2888190` を呼び、その後 `0x7CAEA60(component)` で更新する。
enqueueは借用packetからmotion `+0x38`、別引数から受信時刻（packet `+0x18`）を読み、
native allocationとconstructor `0x2887320` によって値を所有するイベントを作る。
enqueue自体は元packetや時刻引数のpointerを保存しない。timestamp差を `1,000,000` で
割ってmsにする命令と、constructorがmotion/timeをコピーする命令まで確認した。

v4の `apply_knockback` は現在のtick/thread/client/player、ActorOwnerを検証する。
PredictedMovementComponentが有効な場合、上記3関数のRVA/先頭16 bytesとcontrollerの
読めるprefixを確認し、同じenqueue/update経路へ渡す。失敗を直接書き込みで隠さず、
失敗として再試行する。予測componentがない、またはnativeと同じ条件でbypassされる場合は
従来の直接velocity更新を使う。`predicted` はqueueへの受理であり、同じ時点の
StateVectorに既に反映されたことは意味しない。

保持するのはmotion、元のnative受信時刻、DLL内のmonotonicな受付時刻。
解放時のtimestampは「元のnative受信時刻 + 実際の保留時間(ns)」とする。native clockの
epochを推定せず、DLL側clockの絶対値を代入しない。再試行ごとに経過時間を再計算し、
加算overflowは拒否する。native呼び出しの引数envelopeはscalarだけであり、ゲームの
Packet/shared_ptr/STL objectをDLL側で構築・破棄しない。input tick非zero、または
受信timestampが非positiveのpacketはこの試作の対象外として元の処理へ通す。

補正観測: ID161のfactory `0x16BCA30` → dispatcher `0x1185B678` / vtable
`0xE7C0030` / handle `0x1734000`（callback slot `+0x230`）。切断観測: ID5のfactory
`0x16B0AD0` → dispatcher `0x1185AF90` / vtable `0xE7BE678` / handle `0x1723320`
（callback slot `+0x7E8`）。各dispatcherの先頭とvtable entryを確認してフックし、
client handler vtable `0xE7AF110` への到着だけをログへ出して元の処理を呼ぶ。
MovePlayer等を含むすべての補正方式や、サーバーの内部キック理由を観測するものではない。

テストはnative ABIの引数位置・時刻・enqueue→update順序、native queueへの経路選択、
各hitの時刻更新、失敗再試行時の再更新、overflow拒否、Client adapterの転送を追加検証する。
関数prefixはローカルPEとも照合した。実機でprediction分岐が有効だったかはv3ログには
情報がなく、v4の `native_predicted` / `direct` で確認する。Hiveでの切断解消は未確認。

モジュール、Client route、メモリfixture、設定保存とGUIのテストで、8秒前の不適用、
各hitの期限、順序、失敗時の再試行、破棄条件、Presentからの書き込み禁止と隣接値の保持を検証する。

### 解放境界のread-only trace（release-trace-v5、2026-10-30）

実機v4では9件の解放が全てdirect、native_predicted=0だった。通常受信の直接setter
`0x1A0B8C0` はActor `+0x218` のStateVectorにmotionの12 bytesのみを書く。
v5では適用前後とNSL再送直前に、ECSから引いたStateVectorとcached pointerの一致、
capture serial、現在の位置・速度を記録する。native予測を強制したり、解放と確認の
待ち時間を再変更したりする版ではない。経路名でno-component/bypassed/feature-offを区別する。

MovePlayer ID19: dispatcher vtable `0xE7BF740` のentry `+8` → `0x1732C20`、
先頭16 bytes `4C89C14D8B01488B01488B8008070000`、callback slot `+0x708`。
ClientNetworkHandlerの対応関数 `0x133EA10` ではpacket `+0x38` の位置、`+0x44` の回転、
`+0x50` のmodeを `0x200F20` へ渡している。packet vtable `0xE85BFC0`、getId
`0x2ECD950` の `B813000000C3` を照合する。runtime ID `+0x30` が現在のlocal runtimeと
一致するものだけを記録する。on_ground `+0x51` はbool、modeは0–3を検証する。
mode名はpinned headerではNormal/Respawn/Teleport/OnlyHeadRot。
[PlayerPositionModeComponent](https://github.com/LiteLDev/LeviLamina/blob/a0d87a55ea2593d28e63fa1eb6879d373ce95899/src/mc/entity/components/PlayerPositionModeComponent.h)

PlayerAuthInput ID144: vtable `0xE83A360`、getId `0xB26A00` の `B890000000C3` を照合する。
position `+0x38`、delta `+0x48`、128-bit flags `+0x88`、tick `+0xA8` をread-onlyでコピーする。
flagsの上位wordはbits0–1だけを受理し、position/deltaのfinite値も検証する。
tickのoffsetは既に照合済みのpayload先頭 `+0x30` と、同commitのtyped fieldのsize/alignmentから
得たもの。ローカルPEのclone `0x2891DBA` 以降では `+0x30..+0xAF` をscalar領域としてコピーし、
reflection側の初期化 `0x3D4FC93` にも `+0xA8` へのsentinel書き込みがある。
それだけでtickの意味が独立に証明されるわけではなく、型の意味はpinned headerに依存する。
`+0xB0` 以降の所有pointer/containerをtraceへコピーしたり、tickを書き換えたりはしない。
[PlayerAuthInputPacketPayload](https://github.com/LiteLDev/LeviLamina/blob/a0d87a55ea2593d28e63fa1eb6879d373ce95899/src/mc/network/packet/PlayerAuthInputPacketPayload.h)

PAIはgeneral send/sendToServerの元関数へ渡す直前、NSL再送は保存済みoriginal trampolineを
呼ぶ直前を観測する。slot2/slot4がaliasする場合のPAI観測は一度。別関数の場合は双方を観測し、
route、呼び出し中のpacket address、thread、tickで区別する。generalからslot4へ転送された
場合には同じPAIの二つの観測があり得るため、二つのnetwork送信と数えてはいけない。
全PAIがslot4を通るとは仮定しない。packet addressは数値として表示するだけで保存・再利用しない。
MovePlayerは借用shared_ptr-referenceの呼び出し中だけコピーし、その後は元handlerへ通す。
別threadの送信からはimmutableなprofileとpacketだけを読む。ゲームframe/ECSの読み取りは
既存のrender_mutex内に限定する。MovePlayerのlockがbusyなら即座に元handlerへ通す。
成功・失敗どちらの読み取りでもpacketや借用referenceを保持しない。

予定解放付近と実際の解放後の時間窓をatomicに公開し、詳細traceは1窓2,048件までに制限する。
これはnetwork captureではなく、観測seq/threadを付けたnative APIへの投入記録である。
trace cap、lock busy、失敗したレイアウト検証による観測漏れやログI/Oの影響は残る。
境界テストではpacketの不変性、コピーした各field、異なるruntime ID、mode/bool/flagsの
不正値、NaN/Inf、読めないpointer、offset overflow、未対応hostでのprofile gateを検証する。
実機v4の時系列と調査の限界は [Velocity調査](velocity-delay-investigation.md) に記録した。

### 同じactor/worldでの位置reset失効（position-reset-v6、2026-10-30）

実機v5は全6件 `direct:no-component`、`cache_match=1`。21:42:42.975の適用後、
42.976のPAI tick2653へKBが反映され、43.106にlocal MovePlayer mode2を受信した。
43.121のtick2656には補正位置、delta0、HandledTeleport bit37が反映された。
しかし45.120に補正前のKBを再適用し、45.316にDisconnectを受信していた。
setterの参照先不一致や「最初のPAIにKBが反映されない」という説明を、この試験の
根拠として使うことはできない。最初の補正理由はサーバー内部の記録なしでは未確定。

v6は既存のMovePlayer type/profile/layout検証をpacket-only readerへ分離する。
受信threadはmode1/2のruntime ID/位置等のscalarコピーを256件まで保持でき、
Presentがframe lockを使っていても失効通知を落とさず後で処理する。
保持した通知はrender_mutex内で現時点のlocal runtime IDと照合し、mode1/2に限り
Velocityの旧pending motionをcancelled sequenceへ進める。normal/head-only/未知modeや
別のruntime IDはlocal motionを失効させない。

通知の排出は、受信hookがframe lockを取れたとき、capture成功後のmodule更新前、
新規motion受付前、NSL受付前に行う。確認packetの再送自体は引き続き有効なgame tick内だけ。
古い確認はFIFOのまま返し、新しいKBのsequenceに付いた確認はそのKBの適用まで待たせる。
借用packetやactor pointerは通知キューへ保存しない。キューoverflow時は最古通知の
破棄をログへ出す。MovePlayerの元のnative handlerとnative teleport応答は保存する。

同じactor/worldの補正後に旧KBが期限を迎える回帰fixtureを追加した。旧KBの不適用、
確認の保持・送信失敗再試行、補正後の新規KBと確認の期限、respawnとOFF後のcleanup、
mode0/3/未知値の非介入と診断カウンターを検証する。packet fixtureはruntime IDも
コピーされること、未対応hostではpacket-only readerとidentity判定が閉じることを検証する。
実機v5で観測された補正後の再適用を防ぐ変更であり、Hive上の初回補正や切断の解消は未確認。

### 零motionの即時通過（zero-stop-v7、2026-10-30）

受信ログでは、サーバーがKBのほかに零motionのSetActorMotionを速度停止・補正として
送っていた。v6は零motionも通常のKBと同じ遅延キューへ入れていたため、停止が
5秒後まで適用されない経路があった。v7はmotionの各成分が `1e-6` 以下の場合、
native dispatchを止めずに即時通過させ、古い保留KBを失効させる。

これはpacket layoutやnative関数の変更ではなく、既存の検証済み `local_motion` の
値に対する分岐の追加である。零motionをKBの適用として数えないため、遅延時間・
確認応答の順序・予測キューへの投入条件は変えていない。回帰テストでは、
保留KBと確認がある状態で零motionを渡したとき即時ignored・保留0・確認は
FIFOで返ること、保留がない零motionがキューに入らないこと、微小だが非零の
impulseは従来どおり遅延することを確認する。

この修正は零motionの取り扱いだけを直すもので、5秒/8秒遅延のサーバー受理や
Hiveのlagback/kick解消を確認したものではない。

### NetworkStackLatency timestampの内部spoof（internal-ping-v10、2026-10-31）

**v11で撤去済み。** 以下は試作時の記録。サーバーが管理する値からDelay時間を
減算する根拠はなく、値の大きさからns/µs/msを推定することも正当化できない。
現行は `+0x30` を不透明なtimestamp/確認IDとしてコピーし、同じ値を返す。
2026-10-31の2sessionではKB保留・適用とも0のままNSLの書き換え後に切断を観測した。
詳細は [Velocity調査](velocity-delay-investigation.md) のv11記録を参照。

実機ログでは`RakPeer::SendImmediate`が一度も呼ばれず、RakNet ConnectedPingの
spoofはno-opだった。一方`NetworkStackLatency`は`latency_seen`が数分で400件を
超え、実際に使われている。v10はUI設定を追加せず、Delayの値と同じmsを
Delay中のNSL応答の`mCreateTime`（`+0x30`、uint64）から引く。

`NetworkStackLatencyPacket`の`mCreateTime`は`+0x30`、`mFromServer`は`+0x38`
（pinned Solstice headerと一致）。送信hookで`read_latency_reply`のsender/vtable/
dispatcher検証を通ったpacketに限り、`+0x30`をread/writeする。値は`10^12`超をns、
`10^9`超をµs、それ以下をmsとしてms設定値を換算する。書き換え後はSync Replies ONなら
そのコピーを保留し、OFFなら元packetを書き換えて即時送信する。一般sendからslot4への
転送で二重に引かないよう、spoofはslot4の入口だけで行う。

`mCreateTime`がサーバー側のnonceである可能性は排除できない。`nsl_spoofed`が
増加して切断する場合は、内部spoofを無効化する必要がある。この変更はHiveの遅延判定が
NSLの`mCreateTime`を使うことを確認したものではなく、実測で使われている経路へ
対象を移した実験である。

### NetworkStackLatency応答値の保持（opaque-nsl-v11、2026-10-31）

send / sendToServerの両hookからspoof呼び出しを撤去し、SDKの書き換えAPIと
単位推定helperも削除した。KB未受信なら元packetをそのままnativeへ通す。
保留時は検証済み0x48-byteコピーをそのままキューへ入れ、元のslot 4で送信する。
Sync RepliesをOFFにした場合も、既にあるFIFOを新しい応答が追い越さないよう
module側のcleanup判定を通す。opaqueなIDの全bit保持、KB未受信時の通過、
保留・失敗再試行後の同一性、Sync OFFへの遷移を回帰テストで確認する。
サーバーの切断理由payloadは現行ログには含まれず、Flareonの内部判定は未確定。

### Rotations の縦描画・Sprint の修正（2026-10-29）

RotationsModule が `server_yaw` / `server_pitch` の共通要求を保持し、`update_render` で両軸を補間する。pitch だけの変化にも対応し、ON/OFF・world/actor の切り替えで補間状態を破棄する。KillAura 等は角度を要求するだけで、描画処理を持たない。Always では要求元モジュールがなくても実際の視点角度を描画する。

native の `0x1D54950` は `0x1D54BAC` で補間した視点角度を RenderRotation に設定する。既存の検証済み DataDriven callback（`0x558D300`、actor は renderData `+0`）でローカル三人称描画を判定し、Rotations の描画 pitch を callback の間だけ設定する。`RenderPitchScope` が元の pitch を復元する。RotationComponent と隣接する render yaw は変更しない。RenderRotation setter 自体への追加フックは使用しない。CustomModel のアニメーション入力は native の読み取りに戻し、三人称モデルの描画アダプターで同じ Rotations の pitch を受け取る。一人称の腕には実際の視点 pitch を使う。

CustomModel P1 と Rotations は body-yaw 関数 `0x20874B0` を共有する。起動時は CustomModel の P0/P1/P2 接続後に Rotations のフックを有効にする。逆順では MinHook の JMP が P1 の先頭バイトを使う signature を消し、P1 接続とモデル worker の起動が止まる。元の PE では signature が一意に一致し、実行中のフック済み関数では先頭が `E9` に変わって一致しないことを読み取りで確認した。解決済みの元の関数アドレスはフック後も有効で、CustomModel は Rotations の yaw 描画を共有する。

Sprint は ActorDataFlag の直接変更を廃止し、MoveInputComponent（hash `018B1887`）の processed/raw 入力（`+0` / `+0x10`）の SprintDown（bit 8）を更新する。要素幅 `0x64` は `0x283481B`、入力リセット・移動ベクトル `+0x24`・制御フラグ `+0x60` は `0x449810`、raw 入力の更新は `0x43739B1` / `0x4373B1A` と pinned Solstice の構造定義で照合した。Rage/Legit の判定には速度ではなく移動入力を用い、しゃがみ・入力ロック時は解除する。実際の加速と走行可否は native 更新に任せる。ActorDataFlag の読み取りも要素幅を8から16 bytes に訂正した。無効化後のパケットは有効化時の古い状態ではなく、現在の native sprint 状態へ戻す。

メモリ fixture、モジュールテストとビルドで検証する。実ゲームでの走行速度・サーバー受理・視点切り替え時の描画確認は別途必要。

### サーバー向け rotation の修正（2026-10-29）

従来は head/body component と描画用 pitch だけを更新しており、送信パケットの角度は変更していなかった。
既存の送信 hook で PlayerAuthInput（ID 144）の rotation `+0x30/+0x34` と head yaw `+0x44`、
MovePlayer（ID 19）の rotation `+0x44/+0x48` と head yaw `+0x4C` に要求した pitch/yaw を設定する。
PlayerAuthInput の native 検証関数 `0x3C8BA90` と MovePlayer の処理 `0x4359463` で角度の配置を照合した。
対象 vtable RVA はそれぞれ `0xE83A360` / `0xE85BFC0`、getId RVA は `0xB26A00` / `0x2ECD950`。
送信時にも vtable、getId の先頭命令、既存の角度・位置を確認し、一致しない packet は変更しない。

baseTick で成立したモジュール要求だけを yaw/pitch の atomic なペアとして公開する。
Rotations の有効状態や三人称描画には依存せず、描画の補間角度も送らない。
ターゲット消失・入力捕捉・無効化・world clear で送信要求を解除する。Always 単独は視点の描画のみで、packet を上書きしない。
メモリ fixture では両 packet の角度・head yaw、pitch 単独更新、正規化、不正入力拒否と他フィールドの保持を検査する。
実サーバーでの受理・他プレイヤー視点での表示確認は未実施。

### PlayerAuthInput の interact rotation と InputData 位置の修正（2026-10-30）

上記の変更後も、KillAura・Derp などの rotation がサーバー側に反映されないという報告があった。
PlayerAuthInput には `Player Rotation` とは別に、サーバーが攻撃・ブロック操作に使う `Interact Rotation`
（[1.26.52 プロトコル定義](https://github.com/Mojang/bedrock-protocol-docs/tree/automated/1.26.52) の
「The rotation the player is looking for interactions」）がある。従来はここを書き換えていなかった。
pinned Solstice も送信 hook の最後に毎回 `mInteractRots = mRot` を設定している。

メモリ配置は [LeviLamina 26.51 の `PlayerAuthInputPacketPayload`](https://github.com/LiteLDev/LeviLamina/blob/a0d87a55ea2593d28e63fa1eb6879d373ce95899/src/mc/network/packet/PlayerAuthInputPacketPayload.h)
で確認した。Packet 基底 `0x30` の後に rot `+0x30`、pos `+0x38`、head yaw `+0x44`、pos delta `+0x48`、vehicle rot `+0x54`、
analog move `+0x5C`、move `+0x64`、**interact rot `+0x6C`**、camera orientation `+0x74`、raw move `+0x80`、
**InputData（EnumSet<66>、16 bytes）`+0x88`**、InputMode `+0x98` と並ぶ。同じ LeviLamina の `MovePlayerPacketPayload`
（rot `+0x44`、head yaw `+0x4C`、mode/onGround `+0x50/+0x51`）と PlayerAuthInput の rot/pos/head yaw は、上記で照合済みの
1.26.52.3 の値と一致する。1.26.52 のプロトコル定義のフィールド構成も 26.51 と同じ19個である。

- rotation の書き換えで PlayerAuthInput の interact rotation `+0x6C` にも同じ pitch/yaw を設定する。
  元の値が有限・範囲内でない packet は変更しない。camera orientation と vehicle rotation は変更しない。
- Sprint の InputData は `+0x98` を読んでいたが、これは InputMode/PlayMode だった。上位 bit の検査は
  NewInteractionModel（0..2）で通過してしまい、走行中は InputMode に bit 20 などを立てていた。`+0x88` に訂正した。

メモリ fixture に interact rotation の更新と不正値の拒否を追加した。ゲーム実行ファイルでの interact rotation 位置の
逆アセンブル照合と、実サーバーでの受理確認は未実施。

## 攻撃・設置

| 対象 | 確認結果 |
| --- | --- |
| Player の GameMode | `player +0xAA0`。constructor `0x2598C80` は player を mode `+8` に格納 |
| GameMode vtable | base `0xE827560` / derived `0xE827600` |
| buildBlock | slot **6**、base `0x259BC60` / derived `0x25A0810`。BlockPos は Win64 ABI の 12 byte aggregate indirect、face は `r8b`、hand は `r9b`、最後の bool は stack |
| attack | slot **15**、base `0x2599CA0` / derived `0x25A0570`。`r8` の Vec3 pointer を `r9` に移し、attack bool を設定する。interaction 呼び出しではない |
| attack position | callback `0x25B4C50` は渡した Vec3 の xyz を transaction にコピーし、player position との差も計算する。対象 bounds のワールド中心を渡せる |
| inventory slot | callback `0x25B4F6C..0x25B4F73` は player `+0x5B8` の supplies `+0x10` を transaction slot にコピーする。同期呼び出し中だけ選択 slot を変更し、その後必ず元へ戻す |
| HitResult | callback `0x25B4F86` 以降は Level slot **335** で現在の HitResult を取得し、type `+0x18` / facing `+0x1C` / BlockPos `+0x20` / hit position `+0x2C` を読む。`0x25B52AA` の BLOCK 分岐は requested neighbor との差から click position を作る |

GameMode は実行中にも vtable identity、player owner、実行可能領域、slot の関数 RVA と先頭 16 bytes を照合する。RVA は vtable から読んだ関数の識別だけに使い、固定アドレスへ直接 call しない。

Scaffold は空気/液体の target と、空気・液体以外の支持 neighbor（Solstice の `getBlockPlaceFace` と同じ。ガラス・ハーフブロック・階段も含む）を確認し、その neighbor の face center を HitResult に設定してから buildBlock を呼ぶ。HitResult の **scalar prefix 0x38 bytes** を保存・復元し、後続の WeakEntityRef には触れない。Auto Select の slot は呼び出し内で戻さず、選択したまま次の試行で設置し、使わなくなって1秒後に元へ戻す（Solstice の既定 `Full` と同じ考え方）。サーバーによる設置・攻撃の受理はゲーム実行時の確認が必要。

## 描画と残る制約

ClientInstance slot 187 (`0x5DC3730`) は `client +0x1C0` の LevelRenderer を返す。renderer `+0x468` の child、client matrix `+0x420`、child FOV `+0xF58 / +0xF6C` と render origin `+0x9CC` を上流描画式に合わせてコピーする。matrix の identity 初期化は `0x5D97B78` 付近、FOV のコピーは `0x4BABC3` 以降、render origin xyz の参照は `0x4691E91` 以降で確認した。

render origin `+0x9CC` は Solstice の `LevelRendererPlayer_mCameraPos` と同じでカメラ位置にあたり、プレイヤーの頭ではない。ChinaHat はローカルプレイヤーの RenderPositionComponent（Solstice の ChinaHat と同じく、毎フレーム描画される位置）を capture で解決し、Present ごとにコピーで読み直す。tick 位置から直近の移動量の2倍 + 1 block を超える値は捨てて tick 補間に戻る。

combat の eye と描画用 camera origin は別フィールドで保持する。camera profile が成立しない場合は projection を無効にする。render origin は更新・補間を含むため、Present と baseTick の間の描画ずれや三人称切り替え直後の表示は実ゲームでの確認が残る。

可視判定は 12 blocks 以内の候補に限る保守的な block sampling。固体 voxel と取得できない chunk は遮蔽扱い。部分形状の正確な ray collision ではない。ESP/ChinaHat のみの場合はこの可視判定を呼ばない。

`tests/bedrock_gameplay_tests.cpp` は sparse と payload の実ページ境界、古い entity version、不正 vector、重複/不足 hash、読めない pointer、整数 overflow、version gate、tick scope 外の mutation 拒否を検査する。これらはネイティブ攻撃・設置を実ゲームで実行した証拠ではない。
