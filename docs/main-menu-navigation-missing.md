# Main Menu 純正遷移: 解析結果と追加取得項目

**後続更新:** 追加4項目の資料を受領し、純正controller更新Hookと既存callableを使う接続を実装した。
現在の経路と制限は [純正ボタン接続](main-menu-native-navigation.md) を参照。
以下は接続前の不足項目の履歴であり、現在の3メソッドをログ専用とする説明は過去の状態。
一般のScreenEvent/FIFO/ClientInstance quit候補の型は、現在の経路では使わない。

更新: 2026-10-26（日本時間）。ユーザー提供の
[Minecraft_Main_Menu_解析レポート.md](C:/Users/hina/Documents/Codex/2026-10-26/ida-mcp-ida-minecraft-bedrock-edition/outputs/Minecraft_Main_Menu_解析レポート.md)、
同じフォルダーの `verified_addresses.json` / `main_menu_bridge.hpp` を確認した。
添付文書の設計案は解析資料として扱い、安全な呼出しが確定した実装とは扱わない。

## 解析対象と採用範囲

| 項目 | 値 |
| --- | --- |
| モジュール / FileVersion | `Minecraft.Windows.exe` / `1.26.52.3` |
| SHA-256 | `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942` |
| PE ImageBase / SizeOfImage | `0x140000000` / `0x12C02000` |
| 特記事項 | `.levildr` セクションを含む解析ファイル。一般配布ファイルや他revisionとの同一性は未確認。 |
| 検証範囲 | 静的な命令・RTTI・呼出経路。レポートの実行試験は未実施、ロード済みモジュールとの一致も未確認。 |

手元の実ファイルと追加照合し、SHA-256・PE情報9項目、提供された関数先頭バイト17件、vtableスロット13件が一致した。
ZIP内の証拠112ファイルのハッシュも一致。不一致なし。
結果は `out/main-menu/ida-report-validation.json` に記録した。これはディスク上の一致であり、実ゲームのスレッドや呼出しの安全性の検証ではない。

以下のRVA・先頭バイトは**解析データであり、そのままビルドへ接続できるSIGではない**。
SIGの一意性は未検証。新しいSIGや不足型を推測して補わない。
アドレスだけ判明しても、正しいthis・所有権・ABI・実行タイミングが揃うまでは呼ばない。
`VanillaNavigation` の3メソッドは各ボタンと未解決条件をログへ出し、`false` を返す。

| ボタン | レポートで得られた根拠 | まだ接続できない理由 |
| --- | --- | --- |
| Play | `toPlayScreen` RVA `0x3F1DE0` のRTTI宣言。実際のMain Menu callableは `0x4D4A730`、route補助は `0x48F0330`。 | 純正callableの完全な型とイベントdispatchが不明。callableのRCXとcontrollerは異なる。`toPlayScreen` 直呼びでは認証・popup処理を省く可能性があり、同等の経路として未検証。 |
| Settings | `navigateToOptionsScreen` RVA `0x49179F0` のRTTI宣言。Main Menu callable `0x4D50E70` からは `tab=0,b0=true,b1=false,空文字列`。 | modelの実行時取得・基底this調整と安全なUI実行箇所が未確定。純正handler後段のmodel virtual `+0x30` の必要性も未確認。 |
| Quit | 継承Exit callable `0x22BAAC0` → controller virtual `+0x60` → `0x2240F40` → 確認Yes `0x22AE0C0` の経路。 | 元のExitイベントdispatchが未確定。`ClientInstance::quit`候補 `0x5DBD0A0` の引数・戻り値型は推定のため関数ポインタへcastしない。 |

既存の読取実装で `client+0x208` を指していた変数名はSceneFactoryへ訂正した。
これは `MinecraftScreenModel` ではないため、Settings関数のthisとして利用できない。
また、同じ `start_screen` 内でclient・scene・controller等が交換された場合も、取得済みの入力とボタン要求を世代変更で無効化する。
取得したアドレスは比較用の識別子だけに使用し、ゲーム呼出しや所有権の保持には使わない。

## IDA側への追加依頼（対象を限定）

以下は上記ハッシュのバイナリと、同一性を照合した実行プロセスだけを対象とする。
バイナリ全体の再解析は不要。分からない項目は「未確認」のまま返し、型・SIGを推測しない。

1. **元の登録済みイベントを呼ぶdispatcherと取得経路**
   - Play登録key `3372022197` / callable `0x4D4A730`、Settings登録key `4202751130` / callable `0x4D50E70`、継承Exit登録key `116523271` / callable `0x22BAAC0` を入口に、登録先と呼出元を追う。
   - dispatcherのRVA、引数・戻り値の完全な機械ABI、callable/eventオブジェクトのレイアウト、必要なthis調整、存続するMain Menuから**既存callableを取得する**経路を示す。登録keyやFNV値の一致だけをdispatcher APIの保証にしない。
   - Play/Settingsの認証・popupとQuit確認ダイアログが維持される呼出経路を示す。controllerをcallableの代わりに渡したり、外部でnative callableを偽造したりしない。
2. **Main Menuでも実行される安全なUI更新箇所**
   - 純正ボタンの通常クリックで観測したOS thread ID・call stackと、それに一致する非描画のUI update/dispatch関数RVA・全引数・戻り値ABIを示す。
   - その関数のどの前後なら、元処理への再入やscene破棄と衝突せずActionを1件処理できるか、根拠となる呼出順序を示す。window thread、Present thread、world内actor tickがUI threadと同じだとは仮定しない。
3. **controller → modelの取得と寿命**
   - active Main Menuのscene/view/controllerから、各handlerが使うmodelの正しい基底部分へ到達する取得処理・this調整を、当該命令とともに示す。
   - 画面交換、client交換、popup、タイトル復帰、shutdownでの有効化・無効化／破棄タイミングを示す。検査と呼出しの間に破棄されない実行文脈が必要。生ポインタを別threadのキューへ保持しない。
4. **遷移中・終了要求済みの判定**
   - Main Menuで新たなActionを受理しない遷移中／popup／shutdown状態の読取APIまたは実測済みフィールドと、その状態を変更する命令を示す。
   - IApp getter RVA `0x834970` は終了要求の候補根拠。正しいIApp subobject取得、有効期間、確認取消・Yesの動的結果を照合する。setterの観測だけで正常終了完了を保証しない。

各項目の返却には、RVA、アドレス付き逆アセンブルまたはcallsite、型の根拠、静的確認／動的確認／未確認の区分を付ける。
動的に観測したthread・引数・オブジェクトを、静的推定と区別して記録する。
この4項目を満たす元イベントdispatch経路が得られれば、native FIFOや独自STLオブジェクトの受渡しは不要。

native queueを選ぶ場合に限り、追加でFIFOオブジェクト取得・this調整、native functionの構築／破棄ABI、shutdown時の取消／drain完了も必要。
`_queueForMainThread` RVA `0x4954E50` はhidden戻り値型が未確認、FIFO RVA `0x6A2B70` はthis取得が未確認なので、現状では呼ばない。
現在のDLLはLLVM-MinGWでビルドしており、DLL側の `std::string` / `std::function` / `shared_ptr` をゲームのMSVC STLへ渡せるとは扱わない。

## Client側の接続条件

独自ボタンは `Action + session epoch` だけを記録する。
上記で検証したUI箇所でclient・scene・controllerを取得し直し、実行ファイル／ロード済みコード、実thread、Main Menu、未遷移・未終了を確認して元イベントを呼ぶ。
client・scene・controllerが変われば、画面名が同じ `start_screen` でも古い要求を破棄する。
呼出しによってscene/controllerが破棄され得るため、戻った後に旧controllerへ触れない。

描画Hook内からの直接呼出し、未検証のwindow threadからの呼出し、型が不明な関数へのcastは行わない。
停止時は要求の受付と処理を止め、進行中callbackの終了後にアンロードする。
付属 `main_menu_bridge.hpp` の `Stop()` だけでは、すでに要求を取り出した `Pump()` を止められないため、実際のcallback寿命管理との統合が必要。

この文書は残件と実装条件の整理であり、Play / Settings / Quitの実ゲーム動作確認を主張しない。
