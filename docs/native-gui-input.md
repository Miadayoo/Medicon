# GUI中のネイティブマウス・視点入力

2026-10-29。ClickGUI / Social表示中にも視点が動くというユーザー報告への修正。
Windowsのメッセージ、Raw Input API、GetCursorPosの遮断だけでは、ゲーム内部へ既に渡った入力や別の入力生成元を止められなかった。

`BedrockInput`はゲーム側の2つの入口をフックする。

| 入口 | 1.26.52.3 RVA | GUI中の動作 |
| --- | --- | --- |
| `MouseDevice::feed` | `0x361AD0` | originalを呼ばず、ゲームのイベントキューへの追加・移動量の蓄積を止める |
| `CameraDirectLookSystemUtil::_handleLookInput` | `0x66EE3D0` | originalに一時的な `{0, 0}` を渡し、既にキューに入っていた視点移動も止める |

OSのカーソル・ImGuiへの入力は既存経路で継続する。視点用の関数が受け取ったゲーム所有のベクトルやカメラのメモリを直接書き換えない。
ClickGUIの移動キーは従来の通過経路を維持する。Socialとメインメニューは引き続き移動入力も遮断する。
GUIを閉じた後も、ウィンドウスレッドでカーソルを復元し終えるまではネイティブ側の遮断を継続する。
ネイティブ側の遮断はキャッシュされたフォーカス状態に依存しない。

初期化ではPE TimeDateStamp `0x6AB54E37`、SizeOfImage `0x12C02000`（LeviLauncher）または `0x12C01000`（公式）を確認し、両方のシグネチャが一意かつ上記の関数先頭に一致したときだけ有効化する。
ネイティブの遮断が使えない場合、ClickGUIも従来のnative releaseMouseへ戻し、移動のためにマウスをgrabしたままにしない。

ABIの元資料は、既存のSolstice参照リビジョン `1d7808f52ef4ba6e34a7fbcc65294907a7761611` の `MouseHook` と `LookInputHook`。
手元の1.26.52.3 PEで追加の命令列を確認した。

- `MouseDevice::feed` はRCX=this、EDX=button、R8D=buttonData、R9W=x。スタックのy/dx/dyは16bit、motionlessは8bitで読み込まれる。`+0x18`のキューへ20byteの入力を追加し、button=0の場合は`+8/+0xA`へdx/dyを加算する。
- look関数はR8=direct look、R9=2つのfloatへのポインタ。`[R9+4]`を`[R8+8]`から減算し、`[R9]`を`[R8+4]`へ加算した後、角度を正規化する。

`bedrock_input`テストは、PEプロファイル・一意性・関数位置の検証に加え、実際のMinHook detour/trampolineを通して8引数の保存、GUI中のイベント破棄、蓄積済みlookのゼロ化、元ベクトルの保存、閉じた後の新しい入力の復帰を確認する。
実ゲームでの症状解消は、このDLLを使ったユーザー確認が必要。
