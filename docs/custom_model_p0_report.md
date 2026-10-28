# CustomModel P0 診断Hook実装報告

## 最新状態: 統合版の動作をユーザー確認、P1へ進行

2026-10-28 JST。ユーザーから「動いた p1に進めて」と報告・指示を受けた。
P1のF8比較でもOFFでAlexの表示へ戻ることを画面で確認した。
後続の[P1実装・実機報告](custom_model_p1_report.md)を最新状態とする。以下の統合版未確認という記述はその確認前の履歴。

## 履歴: 3人称抑止を確認、一人称用sigも統合してビルド

2026-10-28 JST。近距離/ECSの2本を適用した版で、ユーザーから「三人称は消えた」「1人称がまだ残ってる」と報告を受けた。
続く明示指示により、最初のDataDrivenRenderer `0x558D300`を両world queueと組み合わせ、一人称の腕も省略する実装に変更した。
当初の「腕はバニラのまま」という条件は、この追加指示で変更。**統合版のReleaseビルド成功、関連8テスト全件成功。統合版の実機判定は未完了。**
`addPlayers`にはfallbackしない。DataDrivenRendererは単独のworld抑止入口として使わない。

近距離側は直接actor、ECS側はcomponent先頭、DataDriven側はActorRenderData先頭からchecked copyしたactorを、その場で既存SDKのlocal playerと照合する。
F8 ONかつlocal一致の場合のみorigを省略し、OFF・停止・再入・非対象・読取失敗は元の引数で通す。
3本とも一意に解決し、登録を終えてから有効化する。DataDrivenのABIは根拠のあるvoid(this, context*, renderData*)。
Near/ECS/DataDriven別のcalls/local/skipped/invalidとactor/contextを診断欄に表示できる。
初期OFF、既存native初期化の相対順序、停止時disable→callback drainは維持した。

**表示設定:** CustomModelProbe内の`Show Debug`（保存key `show_debug`）は初期OFF。
F8は抑止だけを切り替え、診断窓を表示しない。Show DebugがONなら、抑止OFFの状態でも診断欄を表示する。
旧`debug_overlay`設定は新しいShow Debugを自動でONにしない。

3本の実MinHook合成native関数で異なるRDX/R8引数、local省略、後続actorの継続、OFF復元・Hook解除を確認。
custom_model_probe / runtime / dll_lifecycle / signatures / bedrock_chat / core / commands / guiが成功した。
体・防具・手持ち・マントの個別網羅、他actor、OFF復元、inventory preview、統合版の腕の抑止は実機未確認。P1には進んでいない。
詳しい根拠と制約は[queue候補報告](custom_model_p0_queue_candidates.md)。

DLLは`D:\code\Medicon\out\build\local\bin\Medicon.dll`。
ゲームを新しく起動してこのDLLを読み込み、ワールド内でF8をONにすると試験できる。
診断を出す場合はCustomModelProbe → Show DebugをONにする。Near / ECS / DataDrivenの3行が統合版の目印。

以下の「新候補は未登録」「旧Hookを無効化したビルド」「F8で窓を表示」は、その時点の履歴であり上記更新前の状態。

## 履歴: addPlayers候補も実機不合格、追加IDA調査完了

2026-10-28 JST。**P0未合格。新しいqueue候補は実行時未登録。P1以降は未着手。**

- 最初のDataDrivenRenderer候補: ユーザー報告で1人称の手が消え、3人称の本体が残った。
- 次のaddPlayers候補（RVA `0x480E6C0`）: 2引数bool callbackとしてHookを実装し、Releaseビルドと関連8テストが成功したが、ユーザー報告では全視点でplayerが残った。自動テスト成功は描画範囲の合格を意味しない。
- 追加IDA調査: addPlayersは近距離collectorの後で遠距離playerを追加する経路だった。新たにqueueRenderEntitiesの近距離側`0x4716E80`と、独立したECS側`0x47172D0`を取得。元EXEで各1件一致し、先頭128 bytesとPE unwind範囲も照合した。
- 取得根拠、引数の相違、再追加経路、未確認範囲は[再調査報告](custom_model_p0_queue_candidates.md)と[候補JSON](../data/signatures/custom_model_p0_queue_candidates.json)。両方を止めれば全描画が消えると確認したものではない。
- 効かなかったaddPlayersの再登録は`add_player_candidate_eligible = false`と`Probe::locate`のgateで禁止。DataDrivenRenderer候補へもfallbackしない。新候補のHookは未実装。
- 現行診断の引数はopaque callbackとactorの2ポインタで、actor memoryの読取はない。SDKのlocal getterで毎回照合し、bool試験detourの省略時はtrueを返す。これは不合格addPlayers試験の機構であり、新しいqueue候補へ流用していない。
- Client起動時は保存設定に関係なくP0をOFFへ戻す。ON/OFF FPSの集計ではsplashや入力捕捉中も除外する。これらの変更を含むRelease DLLをビルドした。

今ビルドを読み込んでもplayerを消すHookは登録されない。取得した新候補を試験したDLLとして案内しない。
旧DLLを読み込んだままのゲームにはソース側の無効化は反映されない。
無効化後のReleaseビルド成功。関連8テスト（custom_model_probe / runtime / dll_lifecycle / signatures / bedrock_chat / core / commands / gui）は全件成功。
実機での全描画抑止、他actorへの影響、OFF復元、腕の保存、inventory preview、性能比較は未完了。

以下は最初のDataDrivenRenderer試験・初回素材準備の履歴。3引数、ActorRenderData読取、保存ON復元、`live_probe_candidate_eligible`という名称はその当時の説明であり、現行実装の説明ではない。

## 過去の試験記録

2026-10-28（日本時間）。**P0診断実装後、実機報告で現候補はNGと判定。P1以降は未着手。**

## 実機結果による訂正

ユーザー報告: **1人称の手は消えるが、3人称で見るプレイヤー本体は消えない。**
したがって、このsigがDataDrivenRenderer::renderに一致する静的根拠と、目的の3人称描画を覆うかは別問題だった。
前回の`local: 1 / skipped: 1`はローカル対象の呼出し省略だけを確認しており、体・防具・手持ち・マントの抑止成功を示していない。

現候補の実行時Hook登録は`live_probe_candidate_eligible = false`で止めた。
次ビルドでは保存設定がONでもこの候補をHookしない。設定がONのパネル表示は`BLOCKED`となり、理由はinject.logへ記録する。
この変更後のRelease DLLを再ビルドし、custom_model_probe / runtime / dll_lifecycleの3テストが成功した。
既に読み込まれた古いDLLのコードは更新されないため、そちらはF8でOFFにする。
以下の操作手順・診断値は、候補を試験していた実装の説明であり、現候補を再有効化する指示ではない。

`actor/v2/DataDrivenRendererRuntime.cpp`の文字列と3参照を追加調査した。
v2経路が3人称の本体を担当するという点は**仮説**で、まだ代替sig/ABIやactor単位の入口を確定していない。
大きな処理全体をHookして他actorまで止める変更や、推測のcontext/カメラフラグは追加しない。

## 今回の実装

| ファイル | 内容 |
| --- | --- |
| `include/medicon/sdk/CustomModelProbe.hpp` / `src/sdk/CustomModelProbe.cpp` | 版・PE情報・既存SignatureRegistryによる一意スキャン、読取失敗時の通過、毎回のローカル判定、atomic診断カウンタ。 |
| `include/medicon/modules/CustomModelProbeModule.hpp` / `src/modules/CustomModelProbeModule.cpp` | VisualのCustomModelProbe、初期OFF、標準F8、Debug Overlay設定。既存設定保存・キーバインド経路を使用。 |
| `src/modules/BuiltinModules.cpp` | 既存登録順序の末尾へP0モジュールを追加。 |
| `src/platform/AutoOverlay.cpp` | 既存HookManagerによる登録、同じ3引数でのorig呼出し、再入/停止ガード、診断パネル、停止時disableと既存callback drainへの参加。 |
| `CMakeLists.txt` | SDKソースとP0テストをビルドへ登録。 |
| `tests/custom_model_probe_tests.cpp` | 異なるPE情報、欠落/複数sig、不正メモリ、非対象、OFF、対象変更、検索中OFFを検証。 |
| `tests/runtime_tests.cpp` | 既存F8ディスパッチと、実MinHook＋3引数の合成native関数で転送/省略/復元を検証。 |
| `docs/custom_model_sigs.md` / `data/signatures/custom_model_candidates.json` | P0の実装状況と静的/動的検証の区別を更新。 |

新Hookは既存native初期化シーケンスの後に追加し、既存Hookの相対順序を変更していない。
local-player bridgeがreadyになっていなければHookを有効化しない。ゲームに入るまでP0もwaitingになる場合がある。
旧ActorRenderDispatcherの7引数ABIとP1の未確定SDKは使用していない。

## 実機での確認手順

1. `out/build/local/bin/Medicon.dll`を従来の方法でMinecraft 1.26.52.3へ読み込む。
2. ワールドに入り、ClickGUIを閉じた状態でF8を押す。設定ではVisual → CustomModelProbe。既存bind機能でキー変更も可能。
3. パネルの`hook ready`を確認する。`waiting`は既存SDK準備待ち、`unavailable`は`%LOCALAPPDATA%/Medicon/inject.log`で理由を確認する。
4. 3人称の背面/正面で、体・防具・メイン/オフハンド・マントを確認する。他プレイヤーとMobも確認する。
5. F8でOFFにして復元を確認する。初回ON以降はOFFでも診断パネルが残り、比較できる。Debug Overlayで隠せる。
6. インベントリプレビューを開き、ON時のlocal/skippedとプレビューの変化を記録する。1人称の腕が変わらないことも確認する。

**実験用候補Hook**。カメラモードやcontextフラグを推測していないため、1人称/UIの専用判別はまだない。
腕を描く別関数はHookしていないが、この入口との関係は実機検証項目に残る。
新規設定では抑止OFF。利用者が保存したON状態は既存configの通常動作で復元される。
初回ON前は追加パネルを出さない。

## 判定・診断値の意味

- FileVersion `1.26.52.3`、PE timestamp `0x6AB54E37`、SizeOfImage `0x12C02000`（LeviLauncher版）または`0x12C01000`（公式インストール版。同じリンクで`.levildr`の1ページがないだけ、RVAは同一。`docs/main-menu-native-navigation.md`）と実行領域内の一意sigを確認する。P0/P1/P2共通（`bedrock_1_26_52_3::known_image`）。不一致時は観測したtimestamp/SizeOfImageをinject.logに出す。実行中イメージ全体のSHA-256一致を主張するものではない。
- 有効時のみActorRenderData `+0x00`をReadProcessMemoryでコピーし、既存BedrockChat bridgeがその場で引き直したlocal playerと比較する。別actor、読取失敗、OFF、停止中はorigへ通す。
- actor/contextは数値の表示IDだけをatomicに記録し、Presentごとに消費する。後のフレームの判定やdereferenceには使わない。
- Calls / Presentは前回の診断消費以降の候補呼出数。local/skipped/invalidも表示する。OFFではlocal取得を行わず、local数/actorは0になる。
- CPUはinspect内のQPC区間の合計。local照合を含むが、orig、Hook進入/退出、末尾集計、パネル描画を含まない。全機能コストの下限の診断値であり、0.3ms目標を満たした証拠ではない。
- カウンタはallocationなしのatomic。複数描画スレッドとPresentが重なる場合、値ごとのフレーム境界は近似になる。
- ON/OFF FPSはワールドsnapshotがあるフレームの件数÷経過秒。切替をまたぐ区間と1秒以上の中断を除外する。同じ視点・条件で比較する必要がある。
- Hook内にファイルI/O、FBXパース、DLL側の動的メモリ確保、UIロック、ゲーム状態の書き換えはない。OFF時も呼出数とQPC集計は続く。

## 現時点のP0結果

| 完了条件 | 結果 |
| --- | --- |
| 3人称で体・防具・手持ち・マントが消える | **NG**。ユーザー報告で3人称のプレイヤー本体が残る。防具等の個別結果は未確定。 |
| 1人称の腕をバニラのまま保つ | **NG**。ユーザー報告で手が消える。現候補を実行時Hook対象から外した。 |
| 他プレイヤー・Mobに影響しない | **実機未検証**。合成native関数の非対象転送テストを用意。 |
| OFFでバニラへ戻る | **実機未検証**。OFFの3引数転送・Hook解除後の元コード復元テストを用意。 |
| 消えない別パスの一覧 | 3人称プレイヤーの描画が残ることを確認。v2候補を調査中で関数一覧は未確定。 |
| インベントリプレビューが同じ関数を通るか | **実機未検証**。呼出数・actor/context・省略結果を記録する必要がある。 |

Release / LLVM MinGWでDLLビルド成功。既存の関連自動テスト8件は成功。
描画ホスト5件は、ライセンス未設定の試験環境で既存のサインイン画面を通常の閉じたHUDと比較して失敗した。
失敗画像を確認し、変更前AutoOverlay.cppを用いる対照DLLでも同じ失敗を再現した。新P0パネルの表示による失敗とは区別する。
制御した実ゲームのFPS ON/OFF比較・全機能の追加CPU時間は**未計測**。画面上の累積FPSは視点/条件が揃っていないため性能の合否には使わない。
**P0の実機合格とは扱わず、P1の直方体描画へは進めていない。**
残る描画があれば観測を起点に別関数だけを限定調査する。P1では補間済みrenderPos/体の向き、native mesh/texture/material型と寿命、material owner等も不足する。

---

## 初回の素材準備記録（以下はHook実装前の履歴）

**後続更新:** ユーザーの依頼でIDA MCPを使用し、DataDrivenRendererの描画入口sig/ABIとP1周辺の9sigを取得した。
現在の結果は [IDA調査報告](custom_model_ida_report.md) を参照。以下は初回素材準備の記録で、入口sig不足という初回の停止理由は解決した。

## ファイル単位の変更

| ファイル | 内容 |
| --- | --- |
| `tools/import-custom-model-assets.ps1` | 指定したディレクトリのFBX 1本・PNGをデータフォルダへコピーし、SHA-256で検証する。既存内容が異なる場合は上書きせず停止。 |
| `tools/inspect-custom-model-prerequisites.py` | 上流5sigのPE内一致数、指定文字列、限定した参照候補を出力する読取専用ツール。 |
| `docs/custom_model_sigs.md` | 対象版・sigの根拠・未採用理由・追加資料の一覧。 |
| `docs/custom_model_p0_report.md` | この段階の結果と検証限界。 |
| `include/medicon/sdk/CustomModelRender.hpp` | 後続IDA調査に基づくopaque型・P0のABI・sig・読取位置。 |
| `data/signatures/custom_model_candidates.json` | 後続IDA調査で取得し、元PEで一意性を確認した9sig。実行時は未登録。 |
| `docs/custom_model_ida_report.md` | 後続IDA調査の根拠と残る未確認点。 |

DLL・CMake・モジュール・新規Hookの実装は変更していない。

## モデル素材

コピー元: `C:\Users\hina\Downloads\neon-t-pose`。

コピー先: `C:\Users\hina\AppData\Local\Medicon\models\neon-t-pose`。

| 内容 | 配置・結果 |
| --- | --- |
| `source/Sprinter.fbx` | `model.fbx`として配置。1,492,796 bytes、SHA-256 `8d036bc5e8ec3536e49a648113af2b281f9d5710b45deb3255cdd2f2994b2210`。 |
| `textures/*.png` | 同じ相対ディレクトリに8枚配置。全ファイルのSHA-256がコピー元と一致。 |
| `asset-manifest.json` | 元の絶対パス、コピー先相対パス、サイズ、SHA-256、素材のみの準備状態を記録。 |

合計9ファイル・12,988,458 bytes（manifestを除く）。元データは変更していない。
再実行コマンド:

```powershell
.\tools\import-custom-model-assets.ps1 -SourceDirectory 'C:\Users\hina\Downloads\neon-t-pose'
```

FBXヘッダーはbinary FBX version 7400。読み出せる文字列には作者環境の
`D:\Output\Exports\ShooterGame\Content\Characters\...` への絶対PNG参照と、それに似た相対参照が残る。
P2では提供された `textures/` への参照解決が必要。現段階ではFBXを編集せず、ロードもしていない。
骨名・クリップ・ソケット・単位・正面方向は未検証。例示値を確定値として `model.json` に記入していない。

## P0完了条件

| 条件 | 結果・理由 |
| --- | --- |
| 3人称の前/後で体・防具・手持ち・マントが消える | **NG / 未実施**。入口sig/ABIは後続調査で取得したが、Hook実装と実機検証はまだ。 |
| 他プレイヤー・Mobに影響しない | **NG / 未検証**。ローカルのみをスキップするHookをまだ接続できない。 |
| OFFでバニラへ戻る | **NG / 未検証**。切替機能は未実装。今回の変更はゲーム描画に介入しない。 |
| 消えない別パスの一覧 | **NG / 未検証**。orig省略を実行できず、残る描画を観測していない。 |
| インベントリプレビューが同じ関数を通るか | **NG / 未検証**。呼出カウンタ・actor・描画contextの実機ログ未取得。 |

P0のF8切替、設定、デバッグオーバーレイ、呼出数/フレームは入口確定後に実装する。
有効なHookを伴わない切替項目は追加していない。

## P1完了条件

| 条件 | 結果 |
| --- | --- |
| 1×2×1直方体が補間済み位置・体の回転に追従 | 未着手。P0が先。 |
| 壁で隠れる深度テスト | 未検証。 |
| ガラス越し・水中での描画 | 未検証。 |
| 昼夜・洞窟の明るさ | 未検証。 |
| 既存textureとUV | 未検証。 |
| 法線・陰影、materialごとの比較 | 未検証。描画APIとmaterial取得SDKが不足。 |

P2～P6は未着手。FBX素材のコピーはランタイム読み込みやカスタムモデル表示の実装ではない。

## 検証・性能

- 9素材のコピー後ハッシュを検証し、同じ入力での再実行でも一致を確認した。
- 上流の旧 `ActorRenderDispatcher_render` sigは対象PEで一致0件。
- 既存の関連4sigは各1件一致。ただしABIは未検証で、新規Hookとして採用していない。
- 指定文字列からの参照候補は監査JSONへ記録し、プレイヤーcontrollerの2参照とTessellatorの1参照だけ近傍逆アセンブルで追加照合した。
- 実ゲームのHook呼出数・FPS ON/OFF比較・追加描画時間・スクリーンショットは**未計測/未取得**。0msやFPS差なしとは報告しない。
- ビルド対象の変更がないため、DLL再ビルドやゲームへのinjectは行っていない。

## ユーザーに用意してほしい資料

初回は **1.26.52.3のプレイヤー/アクター描画入口sigと完全なABI、callsiteの根拠** が不足していたが、DataDrivenRenderer入口について後続のIDA調査で取得した。
`controller.render.player` の参照を持つ関数範囲 `0x4482940` は限定調査の出発点として記録したが、描画入口とは未確定。
P1用のTessellator・メッシュ描画・行列スタック・material取得のSDKも上流に不足している。
具体的な返却項目と既知の候補は [sig調査記録](custom_model_sigs.md) を参照。

次はP0の候補Hookと実機検証から進められる。P1が完了した時点で必ず停止し、結果の確認を待ってからP2へ進む。
