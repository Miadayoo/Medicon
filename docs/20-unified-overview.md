# Medicon Script SDK 0.4

Scripts の cpp を通常のモジュールとして登録し、設定・キー・ON/OFF を ClickGUI と ModuleManager で扱います。
v0.1 の compiler/trust/loader/reload/settings を維持し、既存の Medicon UI とサービスへの C ABI の tail を追加しました。

実装の入口:

- `sdk/include/medicon.hpp`, `medicon/types.hpp`, `medicon/detail/services.inl`: 公開 C++ API。
- `include/medicon/scripts/ScriptRuntime.hpp`, `src/scripts/ScriptExtendedRuntime.cpp`: validation、lifecycle、timers、events。
- `src/gui/ScriptGlassBridge.cpp`: 値変換と既存 Glass/widget への薄い接続。GPU resource を持ちません。
- `src/gui/LiquidGlass.cpp`: 従来の renderer を batch 対応。同じ shader、capture、blur、composition。
- `src/gui/SocialUi.cpp`, `Toasts.cpp`: 通知と既存 sound preference。
- `src/scripts/ScriptStorage.cpp`: preload と非同期 atomic persistence。
- `src/gui/Accessibility.cpp`: Windows 設定を共有。script 専用 preference ではありません。
- `src/gui/ScriptScreen.cpp`: Create、Build、Docs、承認、capabilities、diagnostics、CPU callback metrics。

12 個の実 API サンプルは `sdk/examples` にあります。モック専用の架空ヘッダーは配布しません。
CI も実ヘッダーで構文・コンパイルを検証します。構文検査だけを runtime 検証とは呼びません。

[Getting started](01-getting-started.md) / [API](03-api-reference.md) / [Glass](11-liquid-glass.md) /
[Services](12-extended-api.md) / [UI/Motion](13-animation-and-ui.md) /
[ABI](18-internal-abi-sketch.md) / [Glass mapping](19-existing-liquid-glass-bridge.md) / [QA](16-acceptance-tests-v02.md)
