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

## 上流ソースから訂正した EnTT 配置

上流 `include/entt` の既定値を実ゲームに当てはめると間違うため、既知の component hash を使用する命令列を実ファイルで照合した。

| 対象 | 実ゲームの配置 | 確認した RVA |
| --- | --- | --- |
| Actor の registry / entity ID | Actor `+0x10` / `+0x18` | `0x19E3EA3` / `0x19E3EA7`、`0x20F041` / `0x20F014` |
| registry の pool packed vector | `+0x68 / +0x70 / +0x78`、node 32 bytes、hash `+8`、storage `+0x10` | `0x5DAEBDB..0x5DAEC2D`、`0x20F045..0x20F0A0` |
| component storage の sparse / packed ID / payload vector | `+8 / +0x20 / +0x50` | `0x5DAEC49..0x5DAEC8A`、EnTT base layout と fixture 検査 |
| sparse page | **2048 IDs** (`shr 0xB` / `and 0x7FF`) | `0x5DAEC45` / `0x5DAEC6E` |
| payload page | **128 components** (`and 0x7F`) | `0x5DAEC8A..0x5DAECAB`、`0x20F38C..0x20F3AE` |
| StateVector / Rotation / AABBShape の要素幅 | 36 / 16 / 32 bytes | `0x20F38C..0x20F3AE`、`0x19E3F10..0x19E4062` |
| ActorOwner / ActorType の要素幅 | 8 / 4 bytes | `0x5DAECAB`、`0x2277B7..0x2277CF` |
| RenderPosition の要素幅 | 12 bytes (`lea [i+i*2]` / `shl 2`) | hash `E53C7221`、`0x28A5664..0x28A56C7`、`0x253E28F..0x253E2F4` |

使用する hash は古い `Components/ComponentHashes.hpp` の一覧ではなく、現在の `EnttHashs.hpp` の override と実ファイルで一致する値。StateVector `1B5D5238`、AABBShape `BAC1B3CF`、Rotation `75DF36B7`、ActorType `4F6BA419`、ActorOwner `85B93800`。

head/body componentはbaseTickで更新し、描画時に呼ばれるMobBodyRotation補間getter（RVA `0x20874B0`）とActorHeadRotation補間getter（RVA `0x2087610`）をフックする。最後のYawと現在のYawをgetterのrender alphaで最短角度補間して返し、20 Hzのtick更新間も描画を滑らかにする。これは三人称表示にのみ適用し、一人称ではnative yawを返す。縦向きはRotationComponentだけではモデル表示に反映されないため、同版で確認したRenderRotationComponent（hash `D15944E2`）のpitchを三人称描画時だけ更新する。モデル描画callbackが戻る前にRenderRotationComponentの元の値を復元し、RotationComponentは書き換えない。視点モードが判定できない場合も描画yaw/pitch overrideを適用しない。両yaw getterはMinecraft 1.26.52.3の実行ファイルでcomponent hashと補間処理を確認し、先頭バイトとlocal actorのvtable内にある関数ポインターが一致した場合だけoverrideを有効にする。vtable slotは派生Actorで異なる可能性があるため固定しない。SprintはMoveInputComponentのSprintDownを更新し、PlayerAuthInputPacket（ID 144）のInputDataにSprinting／StartSprinting／StopSprintingを反映する。Strikeは任意方向、Legitは前方への移動入力を条件にする（詳細は下記の修正記録）。パケットのIDとInputData上位bitを検証し、未対応レイアウトは変更せず送る。

コピーした sparse slot を packed ID と完全一致で照合するため、entity の version が変わった古いハンドルは使わない。Actor を列挙するためにゲームの `std::vector` を構築・解放せず、ActorOwner の既存 storage だけを読む。

### Rotations の縦描画・Sprint の修正（2026-10-29）

RotationsModule が `server_yaw` / `server_pitch` の共通要求を保持し、`update_render` で両軸を補間する。pitch だけの変化にも対応し、ON/OFF・world/actor の切り替えで補間状態を破棄する。KillAura 等は角度を要求するだけで、描画処理を持たない。Always では要求元モジュールがなくても実際の視点角度を描画する。

native の `0x1D54950` は `0x1D54BAC` で補間した視点角度を RenderRotation に設定する。既存の検証済み DataDriven callback（`0x558D300`、actor は renderData `+0`）でローカル三人称描画を判定し、Rotations の描画 pitch を callback の間だけ設定する。`RenderPitchScope` が元の pitch を復元する。RotationComponent と隣接する render yaw は変更しない。RenderRotation setter 自体への追加フックは使用しない。CustomModel のアニメーション入力は native の読み取りに戻し、三人称モデルの描画アダプターで同じ Rotations の pitch を受け取る。一人称の腕には実際の視点 pitch を使う。

CustomModel P1 と Rotations は body-yaw 関数 `0x20874B0` を共有する。起動時は CustomModel の P0/P1/P2 接続後に Rotations のフックを有効にする。逆順では MinHook の JMP が P1 の先頭バイトを使う signature を消し、P1 接続とモデル worker の起動が止まる。元の PE では signature が一意に一致し、実行中のフック済み関数では先頭が `E9` に変わって一致しないことを読み取りで確認した。解決済みの元の関数アドレスはフック後も有効で、CustomModel は Rotations の yaw 描画を共有する。

Sprint は ActorDataFlag の直接変更を廃止し、MoveInputComponent（hash `018B1887`）の processed/raw 入力（`+0` / `+0x10`）の SprintDown（bit 8）を更新する。要素幅 `0x64` は `0x283481B`、入力リセット・移動ベクトル `+0x24`・制御フラグ `+0x60` は `0x449810`、raw 入力の更新は `0x43739B1` / `0x4373B1A` と pinned Solstice の構造定義で照合した。Strike/Legit の判定には速度ではなく移動入力を用い、しゃがみ・入力ロック時は解除する。実際の加速と走行可否は native 更新に任せる。ActorDataFlag の読み取りも要素幅を8から16 bytes に訂正した。無効化後のパケットは有効化時の古い状態ではなく、現在の native sprint 状態へ戻す。

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
