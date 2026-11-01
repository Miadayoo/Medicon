# Medicon Script SDK 0.4

## はじめに

Mediconを起動すると `%LOCALAPPDATA%\Medicon` 以下に `Scripts`、`SDK`、
`Cache\build`、`Logs` を作成します。設定パスを上書きしたホストでは、その親フォルダを使います。
SDKとサンプルはDLLに埋め込まれており、起動時のダウンロードは不要です。

1. Visual Studio 2022以降のC++ Build Toolsをインストールします。
   「C++によるデスクトップ開発」、MSVC x64、Windows SDKを選択します。
   管理画面のToolchain setupから公式配布ページを開けます。自動インストールは行いません。
2. `%LOCALAPPDATA%\Medicon\Scripts\HelloWorld.cpp` を作成します。

```cpp
#include <medicon.hpp>
using namespace medicon;
class HelloWorld : public Module {
    void on_enable() override { chat::print("Hello, Medicon!"); }
};
MEDICON_MODULE(HelloWorld, "hello", "Hello World", Category::Utility);
```

3. 保存すると500msの待機後、ワーカースレッドが別プロセスでビルドします。
4. InsertでClickGUIを開き、**Scripts → Script Manager** を右クリックします。
   ソースパス、名前、ソースとDLLのハッシュ、診断を確認して **Approve & Load** を押します。
5. **Scripts**欄の **Hello World** をONにします。ゲーム内チャットを利用できる
   バージョン・ワールドではローカルメッセージを表示します。

各スクリプトは他のModuleと同じ行、ON/OFF、キー割り当て、設定UI、ArrayList、
コマンド、設定保存を使います。登録時のCategoryはメタデータとして保持しますが、
ClickGUIではすべてScriptsに表示します。Script Managerは管理用の行です。

## フォルダと複数ファイル

Scripts直下の各 `.cpp` は独立したモジュールです。直下の `.h/.hpp/.inl` を共有できます。
直下のディレクトリはそれぞれ1モジュールで、内部の全 `.cpp` を再帰的にリンクします。
`MEDICON_MODULE` は1モジュールでちょうど1回だけ使います。
ディレクトリのない `.cpp` とディレクトリ内の `.cpp` は一緒にリンクしません。
シンボリックリンク・ジャンクションをソースに使えません。

`SDK/examples` にHelloWorld、Settings、CustomHUD、Events、MultiFileがあります。
単独ファイルまたはMultiFileディレクトリをScriptsへコピーしてください。
サンプルはビルド・モックホスト・Windows DLL統合テストで検証します。
実ゲーム内の描画・チャットの成否はMCBEの対応プロファイルにも依存します。

ディレクトリには次の形式の `medicon.toml` を置けます。対応する構文は
`[module]`、JSON互換の二重引用符文字列・文字列配列、空行、行全体のコメントです。
未知のキーや未対応のTOML構文は診断として表示します。

```toml
[module]
id = "multi_file"
description = "説明"
required_features = ["render", "local_player"]
```

idはMEDICON_MODULEと一致する必要があります。required_featuresは省略可能です。
利用できない必須機能のあるDLLは実行前に拒否します。任意機能は
`features::is_available("chat")` 等で調べます。対応名はchat、render、local_playerです。
local_playerの機能があっても、ワールド外ではsnapshotはnulloptになります。

## APIとスレッド

公開APIは `SDK/include/medicon.hpp` のDoxygenコメントを参照してください。
サポートする設定型はbool、int、float、Color、std::stringです。

```cpp
Setting<float> radius = setting<float>("radius", "Radius", 4).range(1, 20, .5F);
```

設定宣言とrangeはModuleの構築中だけ使えます。設定値は `get()` で取得します。
Scripts → Script Managerの各スクリプトのSettingsには、文字列を含む全設定の編集UIもあります。
編集の確定時に保存します。通常のドロップダウン欄では既存Moduleと同じ設定UIを使います。
IDは1～63文字のASCII英数字・アンダースコア・ハイフン・ピリオドです。
module_idとsetting_idを変えなければOFF/ON、リロード、再起動をまたいで復元します。
範囲変更後のリロードでは既存の数値を新しい範囲に収めます。設定型を変えた場合は初期値を使います。

on_enable、on_disable、on_tick、on_renderはホストのロックで直列化されます。
on_tickはホストのUIフレームのtickであり、MCBEのbaseTickとは異なります。
TickEvent.delta_secondsで経過時間を取得します。listen<TickEvent/RenderEvent/KeyEvent>は
on_enableで登録します。OFFまたは破棄時にすべて解除されます。
KeyEventはGUIに消費されない入力だけを通知します。

SDKはホストのコールバック内から使ってください。自分で作ったスレッドからは呼べません。
RenderContextはon_render/RenderEventの間だけ有効です。座標・半径・文字サイズは物理ピクセル、
Colorは0～1のRGBAです。描画は既存のImGuiバックエンドを使います。
game::local_playerはゲームスレッドでコピーされた値を返し、生アドレスを公開しません。
chat::printはローカルフィードバックキューへ追加し、ゲームスレッド側の既存ブリッジで表示します。
ゲームのチャット送信やネットワークへの投稿は行いません。

## ビルド・エディタ

正式なコンパイラはMSVC cl.exe、C++20、Windows x64、`/MT` 静的CRTです。
Visual Studio Installerのvswhere、またはDeveloper Command PromptのPATHから検出します。
通常版が見つからない場合はVisual Studio Insiders / Previewも検索します。
複数のMSVCツールセットがある場合は、vcvars64が使う既定のバージョンを選びます。
`/sourceDependencies` が使えるMSVCが必要です。clang-clはこの版では非対応です。
開発・テスト用にLLVM-MinGWのWindows x64 clang++もサポートし、`-static` でリンクします。
clang++がPATHにない場合は `MEDICON_SCRIPT_CXX` に実行ファイルのフルパスを設定します。
コンパイラがない場合もMediconと他のModuleは動きます。

LLVMのライセンスはApache-2.0 WITH LLVM-exception、MinGW側は同梱のライセンスを確認してください。
MSVCはユーザーがインストールしたVisual Studio Build Toolsのライセンスに従います。
コンパイラやWindows SDKの再配布はしません。管理画面に検出パス・バージョン・ライセンスを表示します。

`SDK/compile_commands.json` と `Scripts/.clangd` を自動生成します。
VS CodeでScriptsフォルダを開きclangdを有効化すると、`chat::print` 等のヘッダーを参照できます。
MSVCをclangdで解析する場合はVisual Studioの開発環境からVS Codeを開き、Windows SDKの
include環境も継承してください。ABI境界を本体内部のC++ヘッダーと混在させないでください。

ソース・ヘッダー・SDK・実際のユーザーinclude依存・コンパイラの情報で増分判定します。
キャッシュのDLLも保存されたSHA-256とメタデータで検査します。
Build / Reloadは現在のソースを再評価し、Rebuildはキャッシュを使わずビルドします。
ボタンを押すと待機中の表示に切り替わり、コンパイル中のファイル名とファイル数、DLLのリンク、
入力とDLLの検証、完了まで進捗バーで確認できます。バーは処理工程の完了数を表し、残り時間の予測ではありません。
ビルド中もBuild logを開けます。成功後は承認待ちまたはロード結果、失敗時は停止した工程と理由を表示します。
コンパイラがない場合はビルドできない理由を表示します。Toolchain setupから必要なツールを導入し、
Build / Reloadを押すとコンパイラを再検出します。
失敗しても最後に動いていた版が継続します。ファイルを削除した場合はそのモジュールを解除します。

コンパイラはWindows Job Object内で動作します。各工程の実時間60秒、ジョブCPU時間60秒、
プロセス1GiB・ジョブ2GiB、子プロセス16個、出力4MiBを上限にします。
ソースはファイル単位8MiB、1モジュール2048入力、DLL128MiBを上限にします。
終了時はビルドをキャンセルし、子プロセスを終了します。

## 承認・リロード・障害

ネイティブC++コードはゲームと同じ権限で実行されます。OSのサンドボックスではありません。
未承認DLLはLoadLibrary前に止めます。PEファイルをデータとして解析し、x64、API/ABI、
エントリ、ID、必須機能、非システムDLL依存を検査します。DllMainも承認前には実行しません。
外部DLLや遅延ロードDLLに依存するスクリプトには対応しません。

承認はソースとSDKを含む入力ハッシュと生成DLLハッシュの組に対して保存します。
コードが変わったら再承認が必要です。同じDLLの上書きはせず、ハッシュ名のshadow DLLを使います。
ロード中はファイルの書換え・削除を許可しないハンドルを保持します。
Revoke approvalは承認を取り消し、フレーム境界でそのモジュールを解除します。
承認データが破損している場合は承認なしとして扱います。

フレーム境界で新しい候補を構築し、設定を復元し、旧版をOFFにして新版を登録します。
新版の有効化が成功するまで旧DLLを保持し、失敗したら旧Moduleを再登録して元のON/OFFへ戻します。
旧コールバックの実行が終わり、イベント購読とインスタンスを破棄した後だけDLLを解放します。
GUIが保持する旧Moduleのポインタも消します。

標準C++例外はスクリプト側で捕捉し、当該スクリプトをOFFにして他のModuleを継続します。
ネイティブコードのアクセス違反、無限ループ、破損したポインタ、明示的なプロセス終了は
同一プロセス内では隔離できません。信頼できるコードだけを承認してください。
スクリプトから作成したスレッドをDLL内で実行したままにする設計は非対応です。

診断はソースパス・行・列・severity・コード・メッセージ・ヒントを表示し、
MSVC/clangの未加工出力は `Cache/build/work-*/build.log` に残します。
管理画面のEditor/診断ボタンからファイルを開けます。
監査ログは `Logs/script-manager.log` のJSON Linesです。time_unix_ms、event、detailを記録し、
4MiBで `.1` にローテートします。build_start/success/failed、approve/revoke、load/rollback相当の
info/error、スクリプトのlog、start/stopが記録されます。
設定は既存config.jsonのmodules配下へ保存し、未ロードModuleの保存値も維持します。

### 登録コードがない場合のビルドエラー

cpp がコンパイル・リンクできても、スクリプト登録がなければ Medicon はロードしません。
Module クラスの定義の後に、クラス名に一致する登録を一つだけ書いてください。

```cpp
MEDICON_MODULE(Hello, "hello", "Hello", medicon::Category::Utility);
```

完全な例は `SDK/examples/HelloWorld.cpp`。Create Script の HelloWorld テンプレートからも作成できます。
補助だけの cpp を Scripts 直下へ置くと別モジュールになるため、複数ファイルは同じサブフォルダにまとめます。

旧版では export のない DLL に対して `PE RVA is outside file-backed sections` とだけ表示していました。
修正版は `SCRIPT_REGISTRATION` と登録方法を表示します。実際に壊れた DLL の RVA エラーは、
`SCRIPT_PE_RVA` と対象のテーブル・アドレスを表示します。検査結果と失敗した段階も Build log に残します。

## 互換性と移行

| SDK | api_version | abi_version | Host | MCBE |
|---|---:|---:|---|---|
| 0.4.0 | 400 | 1 | Windows x64, C++20, static CRT | 本体の検証済みプロファイルに従う |
| 0.1.0 (binary compatibility) | 100 | 1 | 旧 prefix table でロード | 同上 |

管理画面はAPI/ABI、実行ホストのMCBEバージョン、現在の機能フラグを表示します。
v0.1 の既存ソース・DLL を維持します。新 API には末尾へ追加した host table を使います。
API 100/400、ABI 1 以外のバイナリは拒否します。再ビルドでソースや DLL が変わった場合は再承認します。
将来の破壊的変更ではAPI/ABIを更新し、この節へ旧→新APIの移行手順を追加します。

## 開発時の検証

`medicon_script_sdk_tests` はモックCホストで公開APIを検証し、Windows以外でもビルドできます。
Windowsの `medicon_script_tests` は公開サンプル12種類の実コンパイル、キャッシュ、外部ヘッダー変更、
clangd（インストール済みの場合）、未承認・ABI不一致・改変DLLの実行前拒否、機能不足、
リロード・ロールバック、例外時の他Module継続、設定復元、in-flight描画のdrainを検証します。
`preview_scripts` は同梱サンプルを一時フォルダでビルド・承認して実際のClickGUIへ登録します。
`.github/workflows/script-sdk.yml` にMSVCの統合テストとポータブルSDKテストを定義しています。
実ゲームへの注入、OS権限の隔離、クラッシュした任意のネイティブコードの復旧を検証するものではありません。

MSVCの依存出力は[MicrosoftのsourceDependencies仕様](https://learn.microsoft.com/en-us/cpp/build/reference/sourcedependencies?view=msvc-170)、
静的CRTの指定は[Microsoftの/MT仕様](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library?view=msvc-170)に従います。
