# Implemented C ABI (not a mock)

正規定義: `sdk/include/medicon/abi.h`。x64 ABI 1、API 400。旧 API 100 も loader が受理します。
export は従来の `medicon_script_manifest` と `medicon_script_entry` です。

| Table | 不変の prefix | 追加 tail |
|---|---|---|
| MediconHostApi | 96 bytes、log から text までの offsets を維持 | frame、shape、glass、UI、theme/input、notification/sound、storage、timer |
| MediconPluginApi | 64 bytes、create/destroy/enable/disable/tick/render/event を維持 | ui |
| MediconScriptManifest | 192 bytes | 変更なし |

offsets は static_assert で固定。API100 DLL には host.size=96、plugin capacity=64 を提示します。
新しい entry は caller capacity と sizeof(table) の小さい分だけ memcpy します。
SDK は host.size で tail を検査してからアクセスします。現時点の拡張 tail は一つのバージョン単位です。
未知の API/ABI は DLL を LoadLibrary する前の PE 検査で拒否します。

Rect/Color/Style/Event は POD コピー。文字列は {data,size} の callback 中の借用か、呼出側が確保した書込 buffer です。
STL、例外、CRT の確保領域を相手側で解放する契約はありません。
timer の void* は不透明な closure token。create/destroy は同じ DLL 内で行い、host はその DLL を pin します。
GPU/フォント/バックバッファへの pointer は ABI にありません。

host は serialized callback lock、enabled/retired、render/UI phase、finite/range、quota を検証します。
reload は新 DLL を検査・承認・構築してから交換し、失敗時は旧 module に戻します。
drain は admission を止め、in-flight callback lock を待ち、disable、timer/UI/toast/pane cleanup、destroy、FreeLibrary の順です。
既存ビルド出力を上書きせず、hash に結びついた別 DLL を使います。

これはネイティブコードのサンドボックスではありません。不正な native pointer や無限ループ・OS API を安全に隔離できる仕組みではありません。
