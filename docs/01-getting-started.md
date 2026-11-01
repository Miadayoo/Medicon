# Script SDK 0.4: Getting started

Windows x64 / C++20。API 400、ABI 1。スクリプトはネイティブ DLL として動きます。
コンパイラは同梱しません。Visual Studio の C++ Build Tools または検出可能な clang++ が必要です。

1. Medicon を起動し、ClickGUI → Scripts → Script Manager を開きます。
2. Create Script から名前とテンプレートを選ぶか、`%LOCALAPPDATA%/Medicon/Scripts/Hello.cpp` を保存します。
3. worker が 500 ms の変更待ち後にビルドします。エラーのファイル・行・列は Manager に表示されます。
4. ソースを確認し、表示されたソースと DLL のハッシュについて Approve & Load を選びます。
5. Scripts に表示された名前をクリックして ON/OFF。右クリックの設定、中央クリックのキーバインドは通常のモジュールと同じです。

```cpp
#include <medicon.hpp>
using namespace medicon;
class Hello : public Module {
    void on_enable() override { chat::print("Hello, Medicon!"); }
    void on_render(RenderContext& render) override {
        auto draw = render.logical();
        draw.glass({24,24,300,86}, Glass::regular().radius(24));
        draw.text({44,54}, "Hello", Color::white(), 20);
    }
};
MEDICON_MODULE(Hello,"my_hello","My Hello",Category::Visual);
```

カテゴリ引数は分類メタデータです。表示先はすべて Scripts です。ID は他のモジュールと重複させず、更新時も維持してください。
`Scripts/MyProject/{main.cpp, helpers.cpp, helpers.hpp}` は一つのプロジェクトです。
`MEDICON_MODULE` はそのうち一つの cpp にだけ書きます。

SDK/include、SDK/examples、SDK/docs は DLL 内の正規のファイルから更新します。これらを直接編集せず Scripts へコピーしてください。
SDK/compile_commands.json と Scripts/.clangd を生成するため、clangd の補完にも使えます。
設定パスを変更したホストでは、その親ディレクトリを保存ルートにします。

[API](03-api-reference.md) / [Glass 対応表](19-existing-liquid-glass-bridge.md) / [UI と Animation](13-animation-and-ui.md)
