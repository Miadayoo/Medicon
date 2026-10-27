# Main Menu SDK 検証記録

調査日: 2026-10-26（日本時間）。**ゲーム実行時の検証記録ではない。**

## 出典と範囲

最初に既存 `BedrockChat`、sig scanner、Hook 管理、`docs/main-menu-dependencies.md` と
`.reference/solstice` を調査した。参照元 [surukotonai/solstice](https://github.com/surukotonai/solstice)
のローカル HEAD と `git ls-remote origin HEAD` はともに
`1d7808f52ef4ba6e34a7fbcc65294907a7761611`。その commit の定義は次の通り。

- [`ClientInstance::getScreenName`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/SDK/Minecraft/ClientInstance.cpp#L121)
  は仮想関数 slot 246 に MSVC 文字列の出力先を渡す。
- [`OffsetProvider.hpp`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/SDK/OffsetProvider.hpp#L172)
  の slot 246 の注記は `1.26.0`。これを 1.26.52 互換とは決めつけなかった。
- [`SigManager.hpp`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/SDK/SigManager.hpp#L84)
  の既存 `ScreenView_setupAndRender` と `MouseDevice_feed` をそのまま参照した。
  `ClientInstance_getScreenName` と `Keyboard_feed` の sig はコメントアウトされている。
- [`SetupAndRenderHook.cpp`](https://github.com/surukotonai/solstice/blob/1d7808f52ef4ba6e34a7fbcc65294907a7761611/src/Hook/Hooks/RenderHooks/SetupAndRenderHook.cpp#L18)
  は常に元関数を呼ぶ。返り値と更新処理を含むため、描画スキップが安全とは確認できない。

不足していた画面識別の最後の経路だけを、既に開かれていた IDA データベースと
ローカル PE の限定的な命令参照で確認した。バイナリ全体の自動解析、関数の一括解析、
IDA データベース変更、SIG の生成・推測は実施していない。後半の照合には既存 LLVM の
`llvm-objdump` と Python 標準ライブラリによる PE 読み取りを使った。

LeviLamina の [AbstractScene](https://github.com/LiteLDev/LeviLamina/blob/main/src-client/mc/client/gui/screens/AbstractScene.h)
と [UIScene](https://github.com/LiteLDev/LeviLamina/blob/main/src-client/mc/client/gui/screens/UIScene.h)
はメソッドの意味を確認する参考にした。そこに記載された配置を移植していない。

## 確認した実行ファイル

| 項目 | 値 |
| --- | --- |
| パス | `C:\Users\hina\AppData\Roaming\levilauncher.exe\versions\1.26.52.03\Minecraft.Windows.exe` |
| FileVersion / ProductVersion | `1.26.52.3` |
| SHA-256 | `47DE440DC290BB5BD2A4CCD79E39C00219A155EA2C23BB961E27E323BD2FD942` |
| PE TimeDateStamp | `0x6AB54E37` |
| SizeOfImage | `0x12C02000` |

ハッシュは調査対象の記録であり、起動時にディスク全体をハッシュしていない。
実装はファイル版 **1.26.52.3 のみ** を受理し、実際の仮想関数アドレス、
観測済み命令列と rel32 呼び出し先を実行中にも照合する。別版や変更された
経路は `unknown` を返し、独自メニューを有効にしない。

既存 sig のファイル内照合結果（実行時スキャンの結果ではない）:

| sig | 一致数 | 一致 RVA |
| --- | --- | --- |
| `MinecraftGame_instance` | 1 | `0x61A4FCA`（rel32 解決前） |
| `MouseDevice_feed` | 1 | `0x361AD0` |
| `ScreenView_setupAndRender` | 1 | `0x4839060` |

## 検証した読み取り経路

以下の RVA は実ファイルで確認した識別用値。コード内でネイティブ関数の呼び出しに使わない。

| 対象 | slot / RVA | 確認した意味 |
| --- | --- | --- |
| ClientInstance 画面名 | slot 246 / `0x5DC7410` | slot 240 で現在の SceneStack を取得し、stack slot 44 の文字列を返す |
| 現在の SceneStack | slot 240 / `0x5DC6DC0` | primary 判定後、SceneFactory 経由でメインまたは client stack を選ぶ |
| primary 判定 | slot 162 / `0x5DC2330` | `client + 0x678` が 0 か確認 |
| SceneFactory | slot 229 / `0x5DC6A30` | `client + 0x208` を返す。提供されたIDA資料で型を訂正。MinecraftScreenModelではない |
| stack 選択補助 | `0x4FFC70` | model `+0x40 == 0` かつ primary のとき client slot 238 を選ぶ |
| メイン stack | slot 238 / `0x5DC6CA0` | `client + 0x1A8` の game に slot 121 を呼ぶ |
| game stack | slot 121 / `0x7F1A20` | `game + 0x1D70` の SceneStack を NonOwnerPointer で返す |
| stack 画面名 | slot 44 / `0x4B28730` | vector `+0x18/+0x20` の最上位sceneポインタを `end - 0x18` から取得し、scene slot 55 を呼ぶ。要素幅は **0x20**、sceneフィールドは要素内 **+8** |
| stack 要素幅 | slot 30 / `0x4B27BF0` | 要素数は `SAR 5`、前要素へ `-0x20`。この命令列も実行中に照合する |
| UIScene 画面名 | slot 55 / `0x485ABD0` | `scene + 0x40` の ScreenView に名前取得処理 `0x484F4A0` を呼ぶ |
| ScreenView 名 | `0x484F4A0` | `[view+0x50] -> +0x8` の定義にある MSVC 文字列 `+0x20` と controller slot 27 の接尾辞を連結 |
| 空の接尾辞 | `0xC8BC0` | 文字列を length 0 / capacity 15 / 空バッファにする純粋な関数。`this` を読まない |
| 実ゲームのタイトルcontrollerの空接尾辞 | `0xD9BB60` | length 0 / capacity 15、`0xE7586B0`の16バイトのゼロ定数をバッファへコピー。関数全体とコピー元定数を検証する |

UIScene vtable は、既存 `ScreenView_setupAndRender` の唯一の direct caller
`UIScene::render` RVA `0x48576B0` を vtable の slot 19 として照合して特定した。
SceneStack vtable は game の `+0x1D70` への代入と、そのオブジェクトを作る
constructor `0x4B1D7C0` の vtable 設定で確認した。

`NonOwnerPointer` の有効フラグは bool。SceneStack constructor は control object の
フラグへ `1` を書き、native getter はそのバイトが 0 でないことを確認している。
読取実装は `== 1` のみ受理し、不明な値を無効とする。参照数の加減算は一切しない。

画面判定は、上記の経路がすべて一致し、最上位 UIScene の定義名が **完全一致で
`start_screen`**、かつ controller slot 27 が確認済みの2種類の空文字列関数のいずれかである場合のみ真。
接尾辞が状態依存の controller、別実装の scene、別 client stack の経路は無効にする。
生の定義名だけで Main Menu と判定しない。

外部オブジェクトはすべて `ReadProcessMemory` でコピーする。ゲームの `std::string` を
DLL の文字列へキャストせず、既存 `read_msvc_string` を使う。ポインタ加算の
オーバーフローを拒否し、名前取得後に stack/control/vector/top/view/definition/controller
へのリンクと名前を再読する。読み取り中の画面切り替えはそのフレームを非表示にする。

## 未確認・不足一覧

- 最新実装のPlay / Settings / Quitは、追加資料で確認した既存callableを専用の純正controller更新Hookから呼ぶ。
  現在の接続と静的根拠は [純正ボタン接続](main-menu-native-navigation.md) を参照。ASLR判定修正後のPlay / Settings遷移はユーザーが実ゲームで確認した。
- 一般のScreenEvent完全型、native FIFO、低レベルquit候補のソース型は未確認で、最新実装では利用しない。
- `1.26.52.3` 以外の revision の画面識別 profile。
- 非 primary client、SceneFactory の別経路、別 SceneStack / UIScene 実装。
- 状態依存の controller 接尾辞を読み取る SDK。現在は空文字列の純粋関数だけを受理する。
- `ScreenView::setupAndRender` 全体をスキップした場合の安全性。更新も含む可能性があり、
  全画面オーバーレイ方式を採用する。
- 実ゲームでの Main Menu、Play、Settings、ワールド参加、切断、タイトル復帰、popup、
  デバイスロスト、リサイズ、入力経路、DLL アンロードの検証。

`tests/bedrock_menu_tests.cpp` は未対応ホストに加え、観測した32バイトの要素を4個持つ128バイトのスタックと実controllerの命令列を含むメモリfixtureで実際の `poll()` を実行する。他画面・タイトル復帰・無効ポインタ・コード変更・空定数の変更も検証する。旧コードでは正常な `start_screen` が拒否されることを再現し、修正後の成功を確認した。fixtureはゲームの実動作とは区別する。

## 実プロセスによる訂正（2026-10-26）

後続のカーソル修正ではボタン反応はユーザー確認できたが、カクつき報告があり、その後の対策は実ゲーム未確認。最新の純正遷移解析と不足項目は [別紙](main-menu-navigation-missing.md) を参照。

ユーザーの不具合報告を受け、実行中の1.26.52.3を読み取り専用で確認した。Main Menuでstackの範囲は128バイト、最上位名は `start_screen`、controllerのslot 27は `0xD9BB60` だった。初版は `end-24` を要素幅24と誤解し、さらに別の空文字列関数だけを受理していたため、正常なMain Menuを無効化していた。要素幅32の根拠は上記slot 30の命令で確認し、修正した。

修正DLLで独自Main Menuが表示されたことはユーザーが実ゲームで確認。その後のカーソル不具合についても実プロセスを読み取り、focus有効・MouseDrawCursor有効・raw no-legacy状態、座標 `(1071,1056)` に対して描画サイズ `1920x1009` を確認した。相対RAWMOUSEの移動量を独自カーソルへ反映し、OS座標による上書きを止める修正を追加。この修正後、ボタン反応とカーソルのカクつきがユーザーから報告された。後続の対策は実ゲーム未確認。
