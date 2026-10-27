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

使用する hash は古い `Components/ComponentHashes.hpp` の一覧ではなく、現在の `EnttHashs.hpp` の override と実ファイルで一致する値。StateVector `1B5D5238`、AABBShape `BAC1B3CF`、Rotation `75DF36B7`、ActorType `4F6BA419`、ActorOwner `85B93800`。

コピーした sparse slot を packed ID と完全一致で照合するため、entity の version が変わった古いハンドルは使わない。Actor を列挙するためにゲームの `std::vector` を構築・解放せず、ActorOwner の既存 storage だけを読む。

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

Scaffold は空気/液体の target と固体の支持 neighbor を確認し、その neighbor の face center を HitResult に設定してから buildBlock を呼ぶ。HitResult の **scalar prefix 0x38 bytes** を保存・復元し、後続の WeakEntityRef には触れない。inventory slot も同じ呼び出し内で復元する。サーバーによる設置・攻撃の受理はゲーム実行時の確認が必要。

## 描画と残る制約

ClientInstance slot 187 (`0x5DC3730`) は `client +0x1C0` の LevelRenderer を返す。renderer `+0x468` の child、client matrix `+0x420`、child FOV `+0xF58 / +0xF6C` と render origin `+0x9CC` を上流描画式に合わせてコピーする。matrix の identity 初期化は `0x5D97B78` 付近、FOV のコピーは `0x4BABC3` 以降、render origin xyz の参照は `0x4691E91` 以降で確認した。

combat の eye と描画用 camera origin は別フィールドで保持する。camera profile が成立しない場合は projection を無効にする。render origin は更新・補間を含むため、Present と baseTick の間の描画ずれや三人称切り替え直後の表示は実ゲームでの確認が残る。

可視判定は 12 blocks 以内の候補に限る保守的な block sampling。固体 voxel と取得できない chunk は遮蔽扱い。部分形状の正確な ray collision ではない。ESP/ChinaHat のみの場合はこの可視判定を呼ばない。

`tests/bedrock_gameplay_tests.cpp` は sparse と payload の実ページ境界、古い entity version、不正 vector、重複/不足 hash、読めない pointer、整数 overflow、version gate、tick scope 外の mutation 拒否を検査する。これらはネイティブ攻撃・設置を実ゲームで実行した証拠ではない。
