# Main Menu の純正ボタン接続

対象は Minecraft.Windows.exe FileVersion `1.26.52.3`。
2026-10-26提供の追加IDA資料と、同一SHA-256のディスク上の実行ファイルを限定して照合した。
SHA-256は `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`。
追加資料の35件の命令列・30件のtable slotは一致し、証拠ZIPの200ファイルもmanifestと一致した。
照合結果は `out/main-menu/ida-additional-validation.json`。

## 呼び出し経路

```text
既存ImGuiボタン
  → Actionとメニュー世代を記録
  → window messageでSDKのローカルキューへ投入
  → Minecraft自身がMainMenu controller updateを呼ぶ
  → 元updateを実行して32bit戻り値を保存
  → 現在のMain Menu・世代・controllerを再検証
  → ゲームが既に保持しているボタンcallableをその場で実行
  → 元updateの戻り値をそのまま返す
```

専用HookはMainMenu controllerの更新 RVA `0x4CF40B0`。
RCXにcontroller、EAXに32bit更新結果という実命令上のABIを使用する。
純正ScreenViewの確認済みcallsiteから戻るアドレス `base+0x483A4DB` だけを受理する。
この呼出箇所では内部実装が所有するcontroller pairからvirtual `+0x10`を呼び、更新後に純正のイベント配送が続く。
OS TIDを固定値やwindow threadから推測せず、実際の呼び出し元と現在の所有contextを照合する。
native呼出しが実施された場合は実行TIDをログへ記録する。

DXGI Present / Present1とD3D描画HookはActionを記録するだけで、Minecraftの画面遷移を呼ばない。
再帰中およびDirectX Hook内からのpumpも拒否する。
描画方式・フォント・背景・カーソルの基盤は既存のまま。

## 既存callableの利用

| ボタン | 登録key | 操作table RVA | invoke RVA |
| --- | --- | --- | --- |
| Play | 3372022197 | `0xE9024C0` | `0x4D4A730` |
| Settings | 4202751130 | `0xE902718` | `0x4D50E70` |
| Quit | 116523271 | `0xE805F70` | `0x22BAAC0` |

具体的なMainMenu controller table `0xE900070` とscene table `0xE8E9080` を照合する。
controller `+0x828` のsentinelから、静的に確認したlist/node/vector配置を読み取る。
key、64バイトの要素幅、操作tableと3スロット、captureのcontroller、invoke先を検証する。
同じkeyに複数handlerがある未知の構成は拒否する。
外部でScreenEvent、ゲームのSTLコンテナ、callable、文字列、shared_ptrを構築しない。
キューはActionと世代だけを持ち、ゲームの生ポインタは保持しない。

純正Playの認証・popup、Settingsの後段処理、Quitの確認処理を含む元handlerを使う。
低レベルの `toPlayScreen` やSettings factory、型が未確定のClientInstance quit候補、IApp setterを直接呼ぶ方法は採用していない。
3つのhandlerの機械上の入力を調べ、invokeへ渡す第2引数のpayloadを読み取らないことを確認した。

## 拒否と停止

版・PE情報・命令・tableが違う場合は接続しない。
実行直前には最上位の正確な `start_screen`、client / scene / controllerの一致、内部実装の所有pair、model pairと参照数、入力抑止byte、終了要求byte、キューの世代と期限を確認する。
入力抑止や終了要求のnonzeroは拒否理由として使う。それらがzeroというだけで全ての状態を安全と判定しない。
未知の配置・無効pointer・画面交換・停止・重複要求も呼ばずにログへ残す。

元updateが画面を変えた場合は要求を破棄する。
実行中の再入・追加要求は拒否し、同じメニュー世代での実行後350msは再要求を受け付けない。
未処理要求は1500msで失効する。
handlerがscene/controllerを破棄する可能性があるため、呼出し後に旧オブジェクトへ触れない。
停止時はSDKの受付を閉じ、世代を無効化し、専用Hookを無効化して進行中callbackの終了を待つ。
既存DLLのprocess-lifetime保持とcallback管理を再利用する。

## 検証範囲

ディスク上の一致と、独立ホスト／合成メモリfixtureによる検証は実ゲームの動作確認と区別する。
追加の `vanilla_navigation` テストでは3操作の実際の機械ABI呼出しを合成コードで実行し、8threadの同時要求、再入、古い世代、期限切れ、停止、native memory破棄後、不正pointer・table・コード・非実行ページを検証する。
これはMinecraftのゲーム関数を呼ぶテストではない。
ASLR判定修正後のDLLで、Play / Settingsの左クリックによる画面遷移はユーザーが実ゲームで成功を確認した。
Quitの取消／Yes、認証popup、各画面からの復帰、最新カーソルの滑らかさを個別に確認した報告はまだない。
未知のruntime構成では、遷移させず診断ログを残す。

### 実プロセスによる初期化修正

初回の接続では、ロード済みEXEのOptionalHeader.ImageBaseがASLR後の実ロードアドレスになっていた。
ディスク上のpreferred ImageBase `0x140000000` だけを受理していたため、SDK初期化がfail-closedになり、ボタンを呼べなかった。
実行中の値を読み取り専用で確認し、preferred値または現在の実ロード先を受理するよう修正した。
版・timestamp・checksum・image size・実行ページ・命令列・tableの照合は維持した。
その実プロセスではコード8箇所が全て一致し、3つの登録keyにはそれぞれ既存callableが1件、table・invoke先・controller captureも一致していた。

全体の自動テスト20/20成功（79.22秒）、ASLR修正後の関連6/6成功（18.72秒）。
実ゲームの確認DLLは `out/build/main-menu-fix/bin/Medicon.dll`。

### 別PCでボタンが反応しない報告（2026-10-27）

別PCでは独自Main Menuは表示されるが、Play / Settingsが反応しないと報告された。
画面判定（`BedrockMenu`）はFileVersionと命令列だけを照合する。
一方、純正遷移（`VanillaNavigation`）はtimestamp・SizeOfImage・CheckSumまで、解析したexeと完全一致を要求する。
解析したexeはLeviLauncherの`versions`フォルダーにあり、`.levildr`セクションを含む。
同じ1.26.52.3でも、公式インストールや別の配布元のexeではヘッダー値が変わり得る。
その場合、独自メニューは表示されて入力を塞ぐのに、ボタンは何も呼べなかった。

修正後は、純正遷移が使えない間は独自メニューを出さず、バニラのタイトル画面へ入力を渡す。
ログには `MainMenu: Main Menu replacement withheld: ...` が出る。
ヘッダー不一致では、実際の値が `MainMenu navigation disabled: Native menu PE header mismatch (...)` に記録される。
フォーカス喪失・メニュー非表示による要求の取消もログに残す。
別PCのexeを受理するかどうかは、そのログの値と命令列の照合結果を確認してから決める。

資料に残る一般のScreenEvent完全型、native FIFO ABI、ClientInstance quit候補のソース型、独立した非描画updateはこの接続経路では利用しない。
それらの未取得事項は `docs/main-menu-navigation-missing.md` に履歴として残す。

診断ログは `%LOCALAPPDATA%/Medicon/inject.log`。
必要に応じて `tools/inspect-main-menu.ps1 -GameProcessId <PID> -Navigation` で、画面・controller・登録済み3handler・入力停止・終了要求を読み取り専用で確認できる。
この診断ツールはゲームの起動・注入・メモリ変更・native関数呼出しを行わない。
