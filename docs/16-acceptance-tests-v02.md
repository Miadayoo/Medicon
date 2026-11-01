# SDK 0.4 acceptance record

検証日: 2026-11-01。Windows x64、LLVM-MinGW clang 22.1.8、Release。
DLL / Preview の全ターゲットをビルドし、CTest **69/69 成功**（204.10秒）。
これは source/build/runtime の検証記録です。実ゲームへの注入は未実施です。

| 範囲 | 検証 |
|---|---|
| 旧 SDK | 凍結した v0.1 headers で別 DLL をコンパイルし、production loader で enable/render/settings |
| ABI | table prefix offsets、caller capacity の canary、未知 API/ABI を offline 拒否 |
| 公開 samples | 12 modules を production compiler で build + PE inspect、cache、clangd |
| Timer | 一度だけ、repeat、cancel、pause coalescing、disable、例外 |
| Storage | ID 分離、UTF-8、path key、容量、restart、atomic backup、破損退避 |
| Events | world join/leave/switch、論理 resize、down-only KeyPressed |
| UI | visibility/focus、RAII、button、progress、stale token、cleanup |
| Glass | D3D11/D3D11On12 の native shader、複数 pane、重なり、device reset |
| Policy | reduced motion / reduced transparency / high contrast と capability/fallback |
| Performance | 1080p/1440p、scale1/1.5、1/5/20 panes の CPU/GPU 時間 |

`scripts` は 12 サンプルに加え Create Script の単体 / MultiFile、重複・不正名拒否まで実コンパイル。
`script_extended` は凍結した旧 DLL と拡張 DLL を production loader へロード。
`script_gui` は実 ImGui の一度だけのボタン発火、古い token、focus、64 slot の上限・解放を確認。
`preview_scripts` は HUD / UI の両方が既存 native publisher に届くこと、非対応診断、graphics recreation を確認。
native Glass の Preview シナリオではアクセシビリティ条件をテスト内で固定し、policy/fallback は別に検証します。

GPU 実測は NVIDIA GeForce RTX 3060、D3D11 offscreen、各条件で warmup 3 frame + 計測10 frame。
結果は `out/build/local/artifacts/script-glass-performance.csv`。
Pipeline statistics で draw 数が `5 + pane数` であることを検証し、共通 downsample 1回 + blur 4回を重複していないことを確認。
CPU 時間は renderer 呼び出し、GPU 時間は timestamp query。実 Minecraft の FPS の測定ではありません。
Preview の画像は `out/artifacts/scripts-v04`。オンライン Docs の公開は未実施で、Docs ボタンは同梱版を開きます。

実 Minecraft Bedrock に注入した UI、SIG/game snapshot、alt-tab、実際の device removal はこの環境では未実施です。
resize / graphics recreation は Preview と offscreen graphics tests で検証し、実 device removal の代用であることを明記します。
MSVC の CI は設定しますが、ローカル LLVM 実行の結果を MSVC の実測とは扱いません。

再現コマンド（Windows）:

```powershell
cmake --build out/build/local --parallel 6
ctest --test-dir out/build/local --output-on-failure
```
