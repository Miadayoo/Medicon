# P0再調査: queueRenderEntitiesの2経路

最新更新: ユーザーから統合版について「動いた p1に進めて」と報告・指示を受けた。
P1直方体の結果は[P1報告](custom_model_p1_report.md)。以下の未確認という記述は当時の履歴。

2026-10-28 JST。**近距離/ECSの2本で3人称が消えるとユーザー報告あり。その後、一人称も消す指示でDataDrivenRendererを追加しReleaseビルド済み。統合版は実機未確認。**
今回の追加IDA調査の結果を記録する。対象はMinecraft.Windows.exe `1.26.52.3`、SHA-256
`47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`。
全sig、先頭128 bytes、callsite、限界は[候補JSON](../data/signatures/custom_model_p0_queue_candidates.json)。

## 試験実装・ビルド

近距離側とECS側に加え、ユーザーが最初のsigとの併用を指示したためDataDrivenRenderer `0x558D300`も既存HookManagerへ登録した。
既存SignatureRegistryで3本とも一意解決した場合だけ有効化する。いずれか欠落・複数一致なら登録しない。
登録途中の失敗では、まだ無効な状態のHookを片付ける。既存native Hookの相対初期化順序は変更していない。

両detourは観察した2ポインタのWindows x64境界を使い、戻り値を使わない呼出元に対してvoidで接続する。
完全なC++元型の回復を主張するものではない。近距離側はactorを直接比較し、ECS側はcallback内で
component先頭の1ポインタだけをReadProcessMemoryでコピーして比較する。
DataDriven側は既に特定済みのvoid(this, context*, renderData*)とActorRenderData +0を使用し、actorをchecked copyする。
一人称の腕を残す当初の条件は、ユーザーの追加指示により変更された。
既存SDKで毎回取得するlocal playerに一致した場合のみ、その1回のorigを省略する。
OFF・停止中・再入・非対象・読取失敗では元の引数でorigへ通す。nativeゲームオブジェクトは書き換えない。

F8は初期OFFで抑止だけを切り替える。診断窓は独立したShow Debug（初期OFF）を明示的にONにした時だけ表示する。
抑止OFFでもShow DebugがONならNear/ECS/DataDriven別の診断値とFPS比較を表示する。
3本の実MinHook合成native関数で、異なるRDX/R8引数の転送、localだけの省略、次actorの継続、OFF・Hook解除後の復元を確認した。
関連8テスト（custom_model_probe / runtime / dll_lifecycle / signatures / bedrock_chat / core / commands / gui）は全件成功。
自動テストはゲーム描画の成功を証明しない。体・防具・手持ち・マントの個別網羅、統合版の腕とプレビューは実機未確認。

出力DLL: `D:\code\Medicon\out\build\local\bin\Medicon.dll`。

## 前の候補が目的を覆わなかった理由

ユーザーの実機報告は「プレイヤーどの視点も非表示にならない」。
前のRVA `0x480E6C0`はaddPlayersのlambdaとしての識別根拠はあるが、
近距離actorを集めた後に、距離のあるプレイヤーを追加する経路だった。

- `0x14469293F`が先に近距離collector `0x14479A820`を呼ぶ。
- `0x14469297C..0x14469298C`は`-10.0f`を用いて下限距離二乗`(D - 10)^2`を作る。
- `0x14480E75C` / `0x14480E75F`は距離二乗が下限以下の場合、追加せず正常終了する。
- 下限定数の元bytesは`00 00 20 C1`、上限側係数3.0の元bytesは`00 00 40 40`。

この経路だけでは近くのローカルplayerを抑止できない。
今回、実行中callbackの呼出数は取得していないため、実機失敗の全原因を特定したとは主張しない。
旧sigは現行resolverの登録対象から除外し、`add_player_candidate_eligible = false`も維持した。fallbackしない。

## 新しい候補

| 項目 | 近距離側 | ECS側の追加経路 |
| --- | --- | --- |
| VA / RVA | `0x144716E80` / **`0x4716E80`** | `0x1447172D0` / **`0x47172D0`** |
| PE unwind範囲（終端を含まない） | `0x144716E80..0x14471727D` | `0x1447172D0..0x144717446` |
| sig長 / 実行可能領域での一致数 | 81 bytes / **1件** | 67 bytes / **1件** |
| RCXの観察 | opaque callback object | 3ポインタを捕捉したopaque closure |
| RDXの観察 | **actorポインタを直接受け取る** | **先頭にactorポインタを持つcomponentへのポインタ** |
| 戻り値の観察 | 呼出元は使わない | 呼出元は使わない |

近距離側はtable `0x14E8DFED0`の`+0x10`から呼ばれる。
`+0x18`の型取得thunk `0x1447172C0`はdescriptor `0x151915D10`を返し、型名は
`.?AV<lambda_78>@?0??queueRenderEntities@LevelRendererCamera@@MEAAXAEBULevelRenderPreRenderUpdateParameters@@@Z@`。
`0x144692C89`で候補配列からRDXへactorを読み、`0x144692C93`で呼び、戻り値を使わず次のactorへ進む。
入口`0x144716EA5`はRDXをRSIへ保持する。ここで`[RDX]`をactorとして取り直してはいけない。

近距離側はrenderer取得、virtual `+0x40` / `+0x50`呼出しの後、ネストしたcallbackへactorのアドレスを渡す。
ネストしたcallbackはqueueRenderEntitiesのlambda_76 / lambda_77という型情報を持ち、設定条件で異なるqueueへ追加する。

ECS側は同じ外側関数`0x144692740`の後段から呼ばれる。
`0x144693C20..0x144693C45`のpool hash `0x85B93800` / `0x8DAB9612` / `0x3890AFF6`は、
既存solsticeのActorOwnerComponent / GlobalActorComponent / GlobalActorRenderComponentと一致する。
これは上流との照合であり、現行native型全体の証明ではない。
callsiteは`0x144693E40` / `0x144693FD5` / `0x144694C2D`。
入口`0x1447172E9`は**`[RDX+0x00]`をRDIへ読む**ので、近距離側と異なり1回の読取が必要。
`0x1447173C2..0x1447173E3`でactorが後段queueに存在するか調べ、なければ
`0x144717403`で挿入helper `0x144716A80`を呼ぶ。
近距離側を省略しても、こちらで再追加される可能性がある。
後段`0x144694932..0x144694938`はそのqueueから別のlegacy描画配列へactorをコピーする。

## 未確認・実装上の制約

- 2本構成で3人称が消えるとのユーザー報告はあるが、各経路の実呼出数・ポインタの画面記録は未取得。
- 体・防具・手持ち・マント全てがこの2経路で抑止されるかは未検証。
- `0x1446940E4..0x1446943F1`には条件付きの別actor配列走査もある。モードと適用範囲は未確定。
- 追加指示で一人称の腕も抑止する方針へ変更。3本統合版の腕の抑止、インベントリプレビューの対象範囲は未検証。
- 正確なC++戻り値型、closureの完全な型、所有権は未確定。確定したのは上記のレジスタ使用と戻り値を使わないcallsite。
- 元のbool callbackの`return true`を流用しない。汎用挿入helperやv2 batch全体を無条件に止めない。
- 未確定のnative STL配置、カメラフラグ、renderer vtable位置を実行時SDKへ追加していない。P1以降は未着手。

## 検証方法

IDA入力SHA-256を照合。背景自動解析が再開していたため停止し、上記関数と近傍だけをデコード・読取した。
全体自動解析は要求していない。sig検索とPE unwind照合はIDA外で元EXEに対して実施。
2本ともIDA先頭128 bytesと元PEが一致し、sigの実行可能領域内一致数1件、開始/終端がPE unwind情報と一致した。
監査出力は`out/custom-model/ida/p0_queue_candidates_raw.json`、`p0_queue_patterns_validation.json`、
`p0_local_pipeline_full.asm.txt`、`p0_nearby_follow_evidence.json`。
