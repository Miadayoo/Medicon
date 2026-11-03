# Minecraft 1.26.60 SDK 更新

2026-11-02。既存実装が使う SIG・SDK の native 経路を、IDA MCP で開かれていた
Minecraft `1.26.60.05`（FileVersion `1.26.60.5`）へ移植した。
初回に残っていた Velocity Delay、Main Menu、Custom Model P0–P3、Alt Manager／Reconnect も更新した。
以下は静的照合・回帰試験と、CustomModel／AltManager の実機確認の記録。
他機能の実ゲーム内操作・サーバー受理の確認とは区別する。

## 対象と出典

| 項目 | 値 |
| --- | --- |
| Solstice | [`cc6463dcadfeadb9049c235134c6b46efdc12b7e`](https://github.com/surukotonai/solstice/tree/cc6463dcadfeadb9049c235134c6b46efdc12b7e) |
| FileVersion | `1.26.60.5` |
| 通常版 SHA-256 | `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a` |
| TimeDateStamp | `0x6AD79EE4` |
| 通常版 SizeOfImage / CheckSum | `0x1383B000 / 0x134D2DEF` |
| LeviLauncher版 SizeOfImage / CheckSum | `0x1383C000 / 0x134D3CBA` |
| LeviLauncher版 SHA-256 | `019f4ee7c82db0e0c187653bc76c61cf75502ff6866a5cc6603fa0c909a386a2` |

`SigManager.hpp`、`OffsetProvider.hpp`、`MinecraftGame.cpp`、Actor/ECS と packet の
レイアウトを参照した。上流 SDK の実装は取り込まず、SIG データを既存 importer で再生成した。
IDA MCP の入力ファイル・image base を確認し、SIG の一致位置と SDK の命令 bytes を
同じ IDB の `ida_bytes.get_bytes` と照合した。IDA の自動解析は未完了だったため、
関数境界の補助には PE の unwind table、命令の確認には Capstone も使用した。
IDB の型・関数名・bytes は変更していない。

Main Menu 非表示の報告後、IDA の元データ、同じ版の `.orig`、現在の起動用 EXE を再照合した。
初回の調査対象は通常版と同じ未変更ファイルで、LeviLauncher 起動後のファイルには
`.levildr` セクションが追加されていた。元の全8セクションは RVA と bytes が完全一致し、
メニュー関連16 entry も IDB と両 EXE で一致した。
記録は `bedrock-1.26.60.05-packaging-evidence.json`。

`BedrockProfile.hpp` に上記2種類を集約し、Menu／Chat／Input／Gameplay／Auth／Custom Model
で同じ timestamp と2種類のサイズを許可する。VanillaNavigation と Reconnect は同じ表から
サイズと checksum の正しい組も照合する。通常版の既存対応を維持して LeviLauncher も追加した。
コード、vtable、オブジェクト寿命の検証は引き続き適用する。
別 timestamp、未知サイズ、異なる packaging の checksum を混ぜた組は拒否する。

CustomModel の追加確認では、P0 の near/ECS/DDR、P1 の描画・マテリアル、P2 の
テクスチャ・mesh、P3 の視点・swing に使う現行ヘッダーの29 SIG を両 EXE に再走査した。
すべて同じ確認済み RVA に一意一致し、対象 bytes も IDB と一致した。
同じ packaging evidence の `custom_model_signature_checks` に結果を記録した。
モデルの読み込み先・視点・アニメーション経路にも Levi 固有のパス／判定は残っていない。
P1 の回帰試験には両 packaging の描画 entry と RIP-relative material の解決、
SIG 欠落・重複、旧 timestamp と未知サイズの拒否を追加した。

CustomModel 非表示の実機画像では、hook は ready だが native meshes `0/17`、PNG `0/3`
のままだった。SIG の一致だけでは保証されない次の native field に旧版の位置が残っていた。

| CustomModel の field | 1.26.52.3 | 1.26.60.5 |
| --- | --- | --- |
| `BaseActorRenderContext+A8` が指す camera の原点 | `+28` | `+20` |
| `TextureData` 内の GPU resource pointer | `+8` | `+0` |
| `ScreenContext` 内の借用 Tessellator pointer | `+B8` | `+B0` |

DDR `59863A9..5986423` と、native texture caller `892052..89208C`、
MeshSubmit `4DEF400..4DEF419` を IDA MCP で追跡し、それぞれの変更を確認した。
`CustomModelBox.cpp` の pose 準備と PNG readiness をその位置へ修正した。
GPU ready の byte `+12`／bit0 と native binding の保持・解放方式は同じ。
証拠は `bedrock-1.26.60.05-custom-model-fields.json`。

実行中のゲームを read-only で調べると、PNG は既に `3/3` だが全 world callback が
Tessellator 検証で失敗し、native meshes は `0/17` のままだった。
native actor draw `5A1CB92..5A1CBA8` は `ScreenContext+B0` を読む。
実際の `+B0` は idle な Tessellator、旧 `+B8` は count `15120`／完了フラグ `05` を持つ
別資源だった。3箇所の借用 Tessellator 取得を共通の確認済み `+B0` へ修正した。

また、独自 PNG をすべて所有している custom model が、無関係な stock binding の
取得まで待つ条件を除いた。stock binding は test box でだけ必要。
native metadata の初期値は有効な空 variant（tag0）で、独自 PNG の送信に使える。

回帰 fixture では正しい camera 原点の直後に NaN を置き、古い読み方では pose 準備が
失敗することを再現した。また GPU の旧位置に ready な decoy を置き、実際の GPU が
pending の間は参照を解放して vanilla を維持し、準備後にだけ native resource を保持する。
旧 Tessellator 位置には実行中と同じ busy に見える decoy を置き、誤参照による停止も再現した。
stock binding が一度も得られない場合でも、独自 PNG だけで準備・描画・解放まで完了する
回帰ケースを追加した。修正前のテスト失敗と修正後の成功を確認している。

修正版を起動中の LeviLauncher `1.26.60.05` へ読み込み、メッシュ `17/17`、PNG `3/3`、
連続した custom draw、描画バッファ拒否0、animation input ready／bulk skinning を確認した。
三人称と一人称でそれぞれ状態を読み、ユーザーから「三人称で表示された」「腕も表示された」
との実機確認を得た。一人称では native readiness と arm readiness もともに true。
通常版は同一コードと両 packaging の回帰試験で確認しており、今回の実ゲーム確認は Levi 版。
関連8テスト成功に加え、新しい通常 DLL と別名の実機確認 DLL の export lifecycle も成功した。

## SIG

64件を再インポートし、**63件は一意一致、0件が未検出、1件が複数一致**だった。
`Actor_canSee` は RVA `0xBC6540` と `0x3534EE0` に一致する。
カタログには出典通り保持するが、既存 `find_unique` はこの結果に usable address を与えない。
Gameplay の可視判定は既存の block sampling を使う。

- [SIG の照合結果](bedrock-1.26.60.05-signatures.json)
- [チャット・入力の命令証拠](bedrock-1.26.60.05-sdk-evidence.json)
- [Gameplay の命令・vtable 証拠](bedrock-1.26.60.05-gameplay-evidence.json)
- [メニュー・モデル・Reconnect・ReplayState の命令証拠](bedrock-1.26.60.05-feature-evidence.json)
- [CustomModel の camera／texture field 修正の命令証拠](bedrock-1.26.60.05-custom-model-fields.json)
- [認証9件と identity の証拠](bedrock-1.26.60.05-auth-evidence.json)

## 基礎 SDK の変更

| 対象 | 1.26.52.3 | 1.26.60.5 |
| --- | --- | --- |
| MinecraftGame primary-client map | `+0x970` | `+0x9D0` |
| primary client の node 内位置 | `+0x28 / +0x30` | `+0x38` |
| client → game | `+0x1A8` | `+0x1C8` |
| client → packet sender | `+0x1D0` | `+0x1F0` |
| client → GuiData | `+0x650` | `+0x668` |
| getLocalPlayer / getBlockSource | slot `31 / 30` | slot `30 / 29` |
| primary 判定 | slot `162` | slot `160` |
| client grab / release mouse | slot `310 / 311` | slot `306 / 307` |
| game の mouse-grabbed byte | `+0x1E8` | `+0x1F8` |
| player → GameMode | `+0xAA0` | `+0xAC8` |
| player → supplies / container model | `+0x5B8 / +0x5A0` | `+0x5E0 / +0x5C8` |
| actor → Level / SynchedActorData | `+0x1D8 / +0x128` | `+0x200 / +0x150` |
| getLevelRenderer | slot `187` | slot `185` |
| client matrix / renderer child | `+0x420 / +0x468` | `+0x438 / +0x480` |
| render camera origin | `+0x9CC` | `+0x9C4` |
| renderer FOV X / Y | `+0xF58 / +0xF6C` | `+0xF50 / +0xF64` |

primary client の新しい native getter は shared_ptr を所有する出力を作るため呼び出さない。
getter `0x8BCFC0` の root→left、key 0、node `+0x38`、client `+8` の lifetime flag を
読み取りだけで再現した。循環・不正 sentinel・読めない pointer・失効を拒否し、
選択後に client/game の vtable と関連 game を検証する。

MouseDevice::feed `0x427650` と camera look `0x6EEBA30` は、既存の詳細 SIG が
それぞれ一意一致し、引数を使う命令も確認した。

Gameplay は GameMode、inventory/action/payload の native constructor/destructor、
actor name、head/body interpolation、container 操作、packet vtable/getId/dispatcher と
static data の位置を更新した。constructor の構造、所有権、field サイズも旧版と比較した。
固定 native 呼び出しは `GameplayProfile.hpp` の確認済み entry と bytes を満たす場合に限る。
旧 RVA が偶然同じ prologue に一致しても、許可リストにないアドレスは使わない。

特に新しい Actor::swing は hand 引数を参照するため、fallback も source/hand を渡す
3引数の ABI に合わせた。

## 独立していた SDK 経路の移植

| 経路 | 1.26.60.5 の変更 |
| --- | --- |
| Main Menu | client slots `243/237/160/227/235`、factory `+228`、game stack `+1DF8`。current-stack selector は native 側で inline 化。SceneStack の32-byte entry、UIScene／ScreenView の名前経路を追跡 |
| VanillaNavigation | controller update `4F17340`、その ScreenView caller `4CB4638`、Play `4F5DE80`／Settings `4F67590`／Quit `26D7E80`。登録 key と既存 callback の所有権を保持。view の input/closing `+480/+485`、game の IApp `+70`、validity `+78`、quit `+A9` |
| Custom Model P0–P3 | active 29 patterns はすべて一意一致。near/ECS `53E9F80/53EA3B0`、DDR `5985040`、world `5376690`。mesh／texture／material の生成・コピー・解放先を対応。options `client+E00` slot172、swing current/previous/flag `+42C/+478/+45D` |
| Alt Manager | 認証9 patterns はすべて一意一致。serializer `4B9A30` は RCX=JSON output、RDX=48-byte auth view。type `+8`／token `+10` を維持。Xbox identity の XUID `+30`、gamertag は `+1F0`。セッション要求 version も `1.26.60` |
| Reconnect | observer `65A34A0`、handler getter `65C06E0`、retry `162920`、SceneStack getter `65B4230`。handler `client+B90`、saved flag `+5A0`、OreUI flag `+5A9`、native destination `+260`。native owner は取得→retry の消費、失敗時だけ解放 |
| Velocity Delay | native motion handler `6BD6210` が使う `ReplayStateComponent` hash `9FC78512`、admission `2CD9D60` に移植。旧 enqueue `2888190` は使用しない |

Velocity は受信 packet を保持せず、motion と input tick を値でキューに保存する。
期限到達時は native constructor `10CACD0` で24-byte motion event を生成し、同じ native allocator の
24-byte control block（vtable `F0CBDA0`、strong/weak とも1）を admission に渡す。
admission は全 return で参照を消費するため、呼び出し後に重ねて解放しない。
生成失敗時の control 解放、admission の各結果、例外時の解放は独立 fixture で検証した。
replay input tick は現在の component tick `+18` まで進め、まだ未来にある server tick は維持する。
tick 0／ReplayState 不在では native handler と同じ直接 velocity 経路を使う。
固定 entry・vtable の確認が失敗した場合だけ capability を無効にして、新規保留を止める。

新しく移植した code guard 40箇所、認証9 entry、描画29 entry の対象 bytes は IDA と一致した。
実ゲームでのモデル表示、メニュークリック、アカウント切り替え／再接続、Velocity の server 受理、
攻撃・採掘・設置・inventory 操作は初回の移植 DLL では未確認だった。
モデルは後続の v2 DLL で、三人称と一人称の腕の表示をユーザーが確認済み。

## AltManager の Use 修正と実機確認

Use が無効だった原因は、`connect_auth()` が旧 `libHttpClient.GDK.dll` のみを探していたこと。
現行 exe の import は `libHttpClient.dll` で、従来名はゲーム内にロードされていなかった。
現行名を優先してロード済みの DLL を解決し、同一 DLL の perform／status／URL／header の
4 exports が揃った場合だけ、認証 hook 3件を有効にする。旧名だけの環境も解決できる。
現行 DLL が不完全な場合に、別の GDK runtime を誤って選択することも防いだ。
新たな libHC のロードやゲーム側の libHC 初期化は行わない。

16:30:31 JST、実ゲーム内で `login bridge ready (libHttpClient.dll)` を確認。
その後ユーザーが Use を押し、16:30:48–50 に Microsoft refresh、Xbox user auth、
Xbox PlayFab token、PlayFab login、Minecraft session の全5段階で HTTP 200 を確認した。
`selected account authenticated for Minecraft; reconnect to apply` と Use 完了イベントも記録された。
初回確認後はゲームが終了したが、最終版を読み込んだ次の起動で再接続まで確認した。
16:38:30 に bridge ready、16:38:32 に Use 認証が完了。
16:38:45 に native multiplayer 要求が選択セッションを使用し、HTTP 200 を返した。
16:38:46 に選択アカウントと一致する有効な認証 token が server login にシリアライズされた。
ユーザーもボタンが `Selected` になったことと、サーバーに接続できたことを確認した。
証拠は [認証 JSON](bedrock-1.26.60.05-auth-evidence.json) の `http_bridge_runtime` に保存した。
認証情報・アカウント識別子・HTTP body は記録していない。

現行名のみ／旧名のみ／両方／現行 API 欠落／両方の API 欠落／両方なしを回帰試験に追加。
認証・AltManager service/UI・CustomModel 描画・Runtime・DLL lifecycle の関連6件が成功した
（最終再検証6/6、10.15秒）。

現在の成果物は `out/build/local/bin/Medicon.dll` と
`out/build/local/bin/Medicon-AltManager-1.26.60.dll`（同一 bytes）。
SHA-256 は `bf091fb733f326d7bd6fb3ef8ab74ed67b9e54e195cf297cd68adb09d5c27658`。
最終版の `Medicon.dll` がゲーム内に読み込まれていることと、上記の再接続処理を確認済み。
初回に Use を確認した版の SHA-256 は
`dfa2e902232a874c4df20ae010927ff3a0285f144573d3e47394eea09986f1ff`。
初回版から最終版への差分は現行 HTTP DLL の export 欠落時の判定と診断だけ。

## 再確認

最終検証：Release 全 target のビルド成功。CTest **70/70成功、200.13秒**。
SDK／モデル／認証／Reconnect／Velocity／DLL lifecycle と、D3D11／D3D12、Preview UI を含む。
ReplayState owner の生成失敗・例外・admission 消費、input tick の引き継ぎも回帰試験に追加した。
Social server の release テスト10件と TypeScript 型検査も成功。
既定 commit で再インポートして JSON / 埋め込みカタログの再現性も確認した。

packaging 修正後も Release 全 target の再ビルド成功。関連 CTest **15/15成功**。
SDK／Main Menu／VanillaNavigation／Custom Model／DLL lifecycle とメニューの Preview を含む。
通常版と LeviLauncher の両方の入力・モデル target 解決、および誤った header 組の拒否を回帰確認した。
CustomModel の追加回帰確認も **8/8成功**（モデル描画・asset・animation・P0、client bridge、Gameplay、Runtime、DLL lifecycle）。
最終の CustomModel 修正後は8/8成功、9.74秒。通常 DLL と実機確認用 v2 DLL の lifecycle も個別成功。

CustomModel 修正時点の成果物：`out/build/local/bin/Medicon.dll`（40,870,400 bytes）。
SHA-256：`6f7a541a652b5e416a24dff7c4820601a065c5dd0a43b47a4c09443a34d7b920`。
差し替え用コピー：`out/build/local/bin/Medicon-CustomModel-1.26.60.dll`（同一 bytes）。
実機で読み込み済み：`out/build/local/bin/Medicon-CustomModel-1.26.60-v2.dll`。
v2 は同じ object/library を別名で link したもので、SHA-256 は
`0763b067fcf83a090e09633e1c667d79d1d3283528c9c339d26bebf82bd3ff94`。

```powershell
./tools/import-solstice-signatures.ps1
python tools/verify-bedrock-signatures.py <Minecraft.Windows.exe> --output out/sig-audit.json
./tools/build.ps1 -Configuration Release -Toolchain LLVM
```

SIG audit は read-only の PE 照合であり、native ABI やサーバー受理を検証するツールではない。
IDA MCP 内では同じ module の `audit(..., use_ida=True)` を使うと IDB bytes も照合できる。
配布 manifest と署名 CLI の既定 MCBE version も `1.26.60` に更新した。
