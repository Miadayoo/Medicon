# Medicon Social フェーズ3 残作業 指示書（Codex 向け）

> **完了（2026-10-27）。** この文書の作業は Claude が行いました。Codex に渡す必要はありません。今の状態は [social.md](social.md) を見てください。この文書の後で、画面の文字は英語に、画面は小さく（影を強く）し、入力欄に入力アニメーションを加えています。

2026-10-27 作成。対象は `D:\code\Medicon`（DLL と Preview）。`social-server` は確認のために起動するだけで、変更しない。

## 0. 進め方

- やるのはフェーズ3の残りだけ。終わったら止まって、§9 の形で報告する。
- 上位の仕様は「Medicon Social 実装指示書」の 1 章（共通ルール）、2 章（共通仕様）、4 章（Part B）、5 章（フェーズ分け）。この文書は、その仕様を今のコードに当てはめた作業リスト。食い違うときは元の仕様に従い、報告に書く。ただし §1.2 の「このプロジェクトで決まったこと」は元の仕様より優先する。
- 始める前に `docs/social.md` と §3 のファイルを読む。今のコードは2つのエージェントが続けて書いたもの。全体を書き直さず、§4 と §5 の点を直す。
- git リポジトリではないので、コミットは不要。

## 1. 守ること

### 1.1 元の仕様の共通ルール

- 既存コードの命名規則、ディレクトリ構成、コードスタイルに合わせる。
- 既存の Hook 管理、SIG Scanner、描画基盤、画面管理を再利用する。新しい Hook は必要最小限にし、追加した場合は理由を報告に書く（フェーズ3では不要のはず）。
- 存在を確認できない既存 API やゲーム内関数を推測で作らない。分からない点には `TODO(social):` コメントを残して報告する。
- DllMain の中で重い処理、スレッドの起動や join、ネットワーク処理をしない。
- 描画スレッドでブロッキング処理（通信、ファイル I/O、JSON パース、待機）をしない。
- ライセンスキー、セッショントークン、メッセージ本文をログに出さない。
- TLS の証明書検証を無効化しない。
- 仕様に書かれていない外部ライブラリを追加するときは、理由を報告する。
- Social は待機中ほぼゼロコストで動き、描画スレッドは通信に一切触れない（inject 後の FPS 低下を別に調べているので、必ず守る）。

### 1.2 このプロジェクトで決まったこと

- 画面は既存の ImGui で描く（ユーザーの決定）。仕様にある D2D・DirectWrite で UI を描く方式は使わない。D2D はワーカースレッドで背景をぼかすときだけ使う。DirectWrite とカラー絵文字は使っていないので、未解決の点として報告に残す。
- Social の文字は `SocialFonts`（Social 専用のフォントアトラス。ワーカースレッドで作る）で描く。ClickGUI 用の `FontManager` のアトラスに Social の文字を足さない。
- 画像などの追加テクスチャは `TextureHost`（D3D11・D3D12・Preview）を通して作る。テクスチャは描画ホストが持つ。
- `include/medicon/gui/ImGuiConfig.hpp` で ImGui の `GImGui` を thread_local にしている（ワーカーでアトラスを作るため）。変えない。
- 描画スレッドで待たない。`std::future` は `wait_for(0)` で終わっているのを確かめてから `get()` する。実行中の `std::async` の future をフレームの途中で破棄しない（破棄すると完了を待つ）。
- テストと Preview では本物の `%APPDATA%\Medicon` を使わない。環境変数 `MEDICON_SOCIAL_DIR` か Preview の `--social-dir` で一時フォルダを指定する。
- フェーズ4以降（Global の送受信、IME、フレンド、DM）は作らない。今のプレースホルダーのままにする。
- コード内のコメントは英語、画面の文字とドキュメントは日本語（既存に合わせる）。
- `social-server` は Medicon フォルダの中に置く（ユーザーの決定）。Node はポータブル版（`social-server\.tools\node-v24.22.1-win-x64`）を使う。

## 2. 今の状態（2026-10-27 13:20 時点）

ビルドは成功し、`ctest` は 27 件すべて成功する。

| 項目 | 状態 | 主な場所 |
|---|---|---|
| `debugLicenseKey` の削除 | 済み。設定ファイルにあれば警告し、読み込み時（ネット側のスレッド）と設定保存時にファイルから消す | `src/social/Config.cpp`、`src/social/SocialNet.cpp` |
| ログイン・ログアウト・再接続・設定保存 | 済み。コマンドを送信スレッドで処理する | `include/medicon/social/Events.hpp`、`src/social/SocialNet.cpp` |
| お知らせの続きの取得（`before`） | 済み | `src/social/SocialApi.cpp`、`src/social/Store.cpp` |
| Social 専用フォント | 済み | `src/gui/SocialFonts.cpp` |
| 追加テクスチャ（D3D11・D3D12・Preview） | 済み | `src/gui/Textures.cpp`、`src/platform/OverlayD3D12.cpp`、`src/preview/Main.cpp` |
| 背景（画面を覆う配置、D2D ブラー、暗幕、サイズごとのキャッシュ） | 一部。画像の読み込み先が違う（§4.1） | `src/gui/SocialBackground.cpp` |
| パネル、開閉アニメーション、サイドバー、ログイン、ホーム、設定 | 済み。Global とフレンドは「準備中です」 | `src/gui/SocialScreen.cpp`、`src/gui/SocialWidgets.cpp` |
| Social ボタン | 一部。見た目、大きさ、バッジが仕様と違う（§4.2） | `src/gui/Overlay.cpp` |
| 通知クリックで画面を開く | 未（§4.3） | `src/gui/SocialUi.cpp`、`src/gui/Toasts.cpp` |
| DLL への組み込み（テクスチャの受け渡し、入力の取り込み） | 済み | `src/platform/AutoOverlay.cpp`、`include/medicon/client/Client.hpp` |
| Preview のモックシナリオ | 済み（`social_login`、`social_home` など） | `src/preview/Scenarios.cpp` |
| 完了条件の確認（実サーバー、アニメーション、FPS） | 未（§4.6〜§4.8） | |

## 3. 読むファイル

- `include/medicon/gui/SocialUi.hpp`、`src/gui/SocialUi.cpp`：描画スレッド側の入口。イベントを受けてストアに反映し、画面と通知を描く。
- `include/medicon/gui/SocialScreen.hpp`、`src/gui/SocialScreen.cpp`：パネル、サイドバー、ログイン、ホーム、設定。
- `include/medicon/gui/SocialWidgets.hpp`、`src/gui/SocialWidgets.cpp`：クリック判定、スクロール、キー入力欄、ボタン類。
- `include/medicon/gui/SocialFonts.hpp`、`src/gui/SocialFonts.cpp`：Social 専用フォント。
- `include/medicon/gui/SocialBackground.hpp`、`src/gui/SocialBackground.cpp`：背景。
- `include/medicon/gui/Toasts.hpp`、`src/gui/Toasts.cpp`：通知ポップアップ。
- `include/medicon/gui/MainMenu.hpp`、`src/gui/MainMenu.cpp`：独自メインメニュー。
- `include/medicon/gui/Overlay.hpp`、`src/gui/Overlay.cpp`：描画の順番と入力の振り分け。
- `include/medicon/gui/Theme.hpp`：`theme::social` の色、寸法、時間。
- `src/platform/AutoOverlay.cpp`：DLL の描画と入力。
- `src/preview/Main.cpp`、`src/preview/Scenarios.cpp`：Preview とシナリオ。
- `tests/social_tests.cpp`、`tests/social_ui_tests.cpp`、`tests/main_menu_tests.cpp`。
- `cmake/EmbedFonts.cmake`：DLL に埋め込むアセット。

## 4. 残作業

上から順にやる。各項目の完了条件を満たしたら次へ進む。

### 4.1 背景画像を、DLL に埋め込んだ back.png から読む（不具合）

今：`SocialBackground::decode()` は `%APPDATA%\Medicon\assets\back.png` を読む。普通の環境にはこのファイルがないので、背景はいつも単色（`theme::social::background_fallback`）になる。Preview でも `Social background not ready; rendering fallback` と出る。

仕様 4.6：「back.png の置き場所は既存アセットの読み込み方法に合わせる。そういう仕組みがなければ `%APPDATA%\Medicon\assets\back.png`」。既存アセット（`mez.png`、`kawai.otf`）は `cmake/EmbedFonts.cmake` で DLL に埋め込み、WIC でメモリから読んでいる（`src/gui/Fonts.cpp` の `IWICStream::InitializeFromMemory`）。`back.png` も同じ CMake で `medicon_embedded_social.hpp`（`medicon::gui::embedded_social::back`）として生成済みだが、どこからも使われていない。

やること：

- `decode()` を、`embedded_social::back` を `IWICStream::InitializeFromMemory` で読む形に変える。`%APPDATA%` から読む処理は消す。
- `medicon_embedded_social.hpp` は大きいので、`SocialBackground.cpp` だけで include する。
- `tests/social_ui_tests.cpp` の `test_background_cache()` は、一時的な APPDATA に PNG を書いて読ませている。画像のバイト列を外から渡せるようにする（例：コンストラクタで `std::span<const std::uint8_t>` を受け取り、既定値を埋め込み画像にする）。テストは自前の PNG で、デコード、ブラー、アップロード、キャッシュ、サイズ変更、再アップロードを今まで通り確かめる。埋め込みの back.png をデコードできることを確かめるテストも足す。
- D2D の `CLSID_D2D1GaussianBlur`（StandardDeviation = 8 × 画面の高さ / 1080、BorderMode HARD）と、D2D が使えないときの CPU のブラーはそのまま残す。縮小してからぼかしている場合は、σ も同じ比率で小さくしているかを確かめる。

完了条件：Preview の `social_home` で fallback のログが出ず、スクリーンショットにぼかした back.png が写る。

### 4.2 Social ボタンを MainMenu に移し、既存のボタンと同じ見た目にする

今：`Overlay::render()` の中に独自のボタンとして書かれている。大きさは 220×58 で、倍率は `min(幅/1920, 高さ/1080)`。白い塗りに青い輪を3重に描き、文字は中央。既存の Main Menu のボタン（`src/gui/MainMenu.cpp` の `draw_buttons`）とは見た目も大きさも違う。既存のボタンは、ずらした影の矩形、白いガラス、白い枠、左のアクセント棒（ホバーでピンク）、DynaPuff の青い文字。バッジもない。Preview の `--main-menu-time` は `MainMenu::render_at()` を直接呼ぶので、今のボタンはそこに写らない。

仕様 4.6：Main Menu の下端中央、下端から 24px 上（24 × 画面の高さ / 1080）。左揃えのボタン群とは別に置く。見た目は既存の Main Menu ボタンと同じ（Shadow・Glow を含む）。未読の DM か未処理の申請があれば、右上に小さなバッジを出す。

やること：

- ボタンを `MainMenu` に移す。`MainMenuFrame` に Social ボタンの矩形を足し、`layout_main_menu_text()` で計算する。幅、高さ、角丸、文字の大きさは既存のボタンと同じにする（`310 × scale`、`62 × scale`、`13 × scale`、`24 × scale`。scale は既存と同じ `短い辺 / 720`）。位置は横の中央で、下端が `size.y - 24 × size.y / 1080`。
- `draw_buttons()` のボタン1個分の描画を関数に切り出し、Play・Settings・Quit と Social で同じものを使う。既存のボタンにない飾りを Social ボタンにだけ足さない。ラベルは `Social`。
- クリック判定は `MainMenu::render_at()` の今の仕組み（中立なフレームを待つ `pointer_armed_`、押した場所と離した場所が同じ、`click_cooldown`）をそのまま使う。結果は `MainMenuAction` とは別に、`consume_social_request()` のような関数で取り出す。`MainMenuAction` に足すと、ゲームの画面へ移る既存の処理に影響するため。
- バッジは `set_social_badge(bool)` のような関数で受け取り、ボタンの右上に小さな丸（色は `theme::social::alert`）を描く。今のフェーズには未読の DM や申請のデータがないので、Overlay からは false を渡し、`TODO(social): phase 5/6` を残す。見た目を確かめるために、Preview でバッジを出す手段（シナリオかオプション）を用意する。
- `Overlay.cpp` の独自ボタン（`social_button_armed_`、`social_button_pressed_` と描画のコード）を消す。Overlay は `main_menu_.consume_social_request()` を見て `social_.open_screen()` を呼ぶ。Social が使えない（`social_.attached()` が false）ときはボタンを出さない。
- `Overlay::collect_text()` に `Social` を足し、Main Menu のフォントに確実に入れる。
- Social 画面が見えている間は Main Menu に空の `PointerInput` を渡す、という今の挙動は残す（画面を閉じたときのクリックでメニューのボタンが押されないように）。
- `tests/main_menu_tests.cpp` に、Social ボタンの位置（横の中央、下端から 24 × 高さ / 1080）と、左のボタン群と重ならないことのテストを足す。16:9、4:3、21:9、小さいウィンドウで確かめる。

完了条件：`--main-menu-time 2` のスクリーンショットに、既存のボタンと同じ見た目の Social ボタンが下端中央に写る。バッジ付きの見た目も確かめられる。クリックで Social 画面が開く（Preview のシナリオか入力テストで確かめる）。

### 4.3 通知をクリックしたら Social 画面を開く

今：`ToastManager::consume_click()` はクリックされた通知を返すが、どこからも呼ばれていない。`docs/social.md` にも「対象画面への遷移は未接続」とある。

仕様 4.7：クリックはメニュー中（カーソルが見えているとき）だけ有効。Social 画面を開いて、該当する DM、フレンド画面、ホームのいずれかを表示する。ゲーム中は当たり判定を持たない。

やること：

- `SocialUi::render()` で `toasts_.render()` の後に `consume_click()` を見て、通知の種類に合わせて画面を開く。
  - お知らせはホームの「お知らせ」タブ、Change Log はホームの「Change Log」タブを開く。開く先は `Toast` に持たせ、タイトルの文字列では判定しない。`SocialScreen` にタブを指定して開く関数を足す。
  - System（認証の失敗など）は Social 画面を開く。キーがないか認証に失敗していればログイン画面が出る。別の場所で接続された（Replaced）ときはホームが開き、サイドバーの「再接続」が見える。
  - DM とフレンドは種類だけ用意して `TODO(social): phase 5/6` を残す。フレンドはフレンド画面（準備中）を開く。
- クリックできるのは、メインメニューと ClickGUI を開いているとき（`in_menu()`）だけにする。これは今の `ToastManager` の挙動のまま。
- ゲーム中に ClickGUI を開いて通知をクリックすると、ゲーム中に Social 画面が開く。入力の扱いは §4.4 で確かめる。
- `tests/social_ui_tests.cpp` に、クリックすると画面が開き、正しいタブになるテストを足す。

完了条件：メインメニューでお知らせの通知をクリックすると、ホームのお知らせタブが開く（Preview のシナリオで確かめられる）。

### 4.4 Social 画面を開いている間の、下にある UI

今：

- メインメニュー：Social 画面が開き切って背景が画面全体を覆っていても、Main Menu（毎フレーム動く背景を含む）を描き続けている。
- ゲーム中（ClickGUI を開いて通知をクリックした場合）：HUD と ClickGUI に本物のポインターを渡しているので、パネルの裏の ClickGUI がホバーやクリックに反応する。

やること：

- `SocialUi` から `SocialScreen::opaque()` を見られるようにし、`Overlay::render()` を次のようにする。
  - 開き切っている間は、Main Menu、HUD、ClickGUI を描かない。
  - 見えている間（開閉のアニメーション中を含む）は、それらに空の `PointerInput` を渡す。
  - Main Menu の描画を飛ばした後も、画面を閉じた直後のクリックで Main Menu のボタンが押されないようにする（`pointer_armed_` が中立なフレームを待つ状態に戻ること）。必要なら `MainMenu` に明示的なリセットを足す。
- Social 画面が開いている間は、ClickGUI の開閉キーを含むキー入力を Social が先に受け取る（`Overlay::key_event()` はもうそうなっている）。Esc は Social 画面を閉じる。ゲームにキーが渡らないことを確かめる。
- `Client::capturing_input()` と AutoOverlay の `set_open(...)` は Social 画面を含んでいる。ゲーム中に Social 画面を開いたとき、カーソルが出てゲームに入力が渡らないことを確かめる。DLL のテストで確かめられない部分は、ユーザーにゲーム内で確かめてもらう手順を報告に書く。

完了条件：メインメニューと、ゲーム中に ClickGUI から開いた場合の両方で、Social 画面の裏の UI が反応しない。開き切った状態で Main Menu の背景の計算が動いていないことを、計測で確かめる。

### 4.5 初めて開いたときの準備を先にしておく

今：`SocialUi::render()` は、Social 画面が見えているときか通知があるときにしか `SocialFonts::update()` と `SocialBackground::update()` を呼ばない。初めて開いたときにフォントの読み込みとアトラス作り、画像のデコードとブラーが始まるので、開いてからしばらく（数百 ms）文字が出ず、背景が単色になる。

やること：

- メインメニューを表示している間に、ワーカースレッドで先に準備しておく。対象は、フォントの読み込み、`SocialScreen::request_glyphs()` の固定の文字列のアトラス、今のウィンドウサイズの背景。描画スレッドでするのは、future の確認とアップロードだけにする。
- 準備が終わった後の毎フレームの追加処理は、今と同じくほぼゼロにする。
- 先に読み込む分だけメモリを使うので、その量（フォントファイルと画像）を報告に書く。

完了条件：メインメニューを2秒表示してから開くと、開くアニメーションの最初のフレームから、文字とぼかした背景が出る。準備の間も、描画スレッドで長いフレームが出ない。

### 4.6 実サーバーで、ログインからホームまでを確かめる（完了条件1）

今：Preview の `social_live` は、`license.bin` がある前提で「オンライン」になるのを待つ。`debugLicenseKey` がなくなったので、ログイン画面を通らないとオンラインにならない。

やること：

- `social_live` を、ログイン画面から始まる流れに直す（分けた方が分かりやすければ `social_live_login` などを足す）。
  1. `--social-dir` の一時フォルダに `serverUrl` だけの `social.json` を置き、`license.bin` はない状態で始める。状態が NoLicense になるのを待つ。
  2. `open_screen()` を呼び、ログイン画面が出ていることを確かめる。
  3. キーは環境変数 `MEDICON_SOCIAL_TEST_KEY` から読む。ない場合は、キーを含まないメッセージで失敗させる。`social_login_success` と同じく `WM_CHAR` で入力し、Enter を押す。
  4. オンラインになるのを待つ。`--social-dir` に `license.bin` ができていて、中にキーがそのまま入っていないことを確かめる。
  5. お知らせと Change Log が届くのを待ち、`home_news` を撮る。タブをクリックして Change Log に切り替え、`home_changelog` を撮る。カードが新しい順に並んでいることも確かめる。
  6. お知らせを 25 件投稿しておき、一番下までスクロール（`WM_MOUSEWHEEL`）すると 20 件より多く読み込まれ、最後に 25 件になることを確かめる。
  7. その後は今の `social_live` と同じ確認を続ける（`news:post` で通知が出る、切断から再接続する、revoke でログイン画面に「このキーは無効化されています」が出る）。外のスクリプトと合わせるために、各段階で `milestone …` を出す今の形を残す。
- 次の流れも確かめる。
  - 同じ `--social-dir` で Preview を起動し直すと、ログイン画面を出さずにオンラインになる（`license.bin` を使う）。
  - 設定でスイッチを切り替えると `social.json` の `notify` が更新され、ほかの項目は残る。スライダーで `inGameOffsetY` が変わる。
  - ログアウトすると `license.bin` が消え、ログイン画面に戻る。
  - 違うキーでは「キーが違います」、revoke したキーでは「このキーは無効化されています」、BAN したユーザーでは「このアカウントは利用停止されています」、サーバーを止めると「サーバーに繋がりません」が出る。
- これらを1回で流すスクリプトをリポジトリに置く（例：`tools/social-e2e.mjs`。ポータブル版の Node で動かす）。スクリプトは、使い捨てのサーバーを起動し（§7）、キーを発行し、お知らせと Change Log を投稿し、Preview のシナリオを動かし、`milestone` に合わせてサーバーの停止、再起動、revoke、BAN をし、最後にサーバーを止めて一時データを消す。
- キーは管理 CLI の出力から変数に取り込み、画面、ログ、報告に出さない。Preview には子プロセスの環境変数で渡す。

完了条件：スクリプト1回で、キーの入力、オンライン、ホームのお知らせと Change Log、続きの読み込み、設定の保存、ログアウト、各エラー表示までが通る。キーはどこにも出力されない。

### 4.7 開閉アニメーションを確かめる（完了条件2）

今：`social_animation` は Esc を押してすぐ開き直すだけで、途中のフレームを撮っていない。

やること：

- 固定の時間刻み（例：`--fixed-dt 0.01`）で、開くときの 0・60・120・180 ms と、閉じるときの 0・40・80・120 ms を撮るシナリオにする。
- 数値でも確かめる。仕様 4.6 の通り、開くときは 180 ms で不透明度が 0 から 1、拡大率が 0.96 から 1（ease-out cubic）。閉じるときは 120 ms。閉じ始めたらクリックが効かない。開く途中で Esc を押すと、その位置から閉じる。テストは `tests/social_ui_tests.cpp` に足す。

完了条件：途中のスクリーンショットで、拡大とフェードが見える。数値のテストが通る。

### 4.8 FPS を測る（完了条件3）

やること：

- Preview の `--gpu-timing --gpu-frames 600`（シナリオの後に指定フレーム数を計測する。`src/preview/Main.cpp` の `run_gpu_timing`）を 1920×1080 で使い、次を比べる。
  - (a) メインメニューだけ
  - (b) Social のホームを開き切った状態
  - (c) ホームをスクロールしている間
  - (d) 通知が3件出ている間
- 平均だけでなく p95 と一番長いフレームも比べる。初めて開いたときのフレーム（§4.5 の準備の前後）も測る。必要ならシナリオを足す。`tools/measure_ui.ps1` も使える。
- 目安：(b) と (c) が (a) より平均で 0.5 ms 以上遅くならない。ウォームアップ後に、中央値の2倍を超えるフレームが出ない。Social 画面を閉じて通知もないときの Social の処理（`debugTiming` の `social` の値）は、ほぼゼロ。目安を満たさなければ原因を調べて直すか、報告に書く。
- ゲーム内の計測はユーザーが行う。`social.json` の `debugTiming` を `true` にして、`inject.log` の Perf 行の `fps` と `social` を、Social を閉じたときと開いたときで比べる手順を報告に書く。

完了条件：(a)〜(d) の数値が報告にあり、目安を満たすか、満たさない理由が書いてある。

### 4.9 ドキュメント

- `docs/social.md`：背景画像（埋め込み）、Social ボタン、通知のクリック、先に準備すること、E2E スクリプトの使い方、FPS の結果を、実装に合わせて書き直す。「ビルドと測定」の Visual Studio 2022 のコマンド（`cmake --preset windows`）は、動くことを確かめたものだけ残し、§6 のローカルの手順も書く。
- `README.md` の Social の節と `assets/README.md`（back.png は `SocialBackground` が埋め込みから読む）を合わせる。
- この文書（`docs/social-phase3-codex.md`）は、作業が終わったら消すか、「完了」と書き足す。

## 5. 見直す点

作業の途中で、次も確かめて直す。直さなかったものは報告に書く。

1. 倍率：仕様 4.6 は「サイズにはすべて scale = 画面の高さ / 1080」。今のコードは、画面（`SocialScreen.cpp`）も文字（`SocialUi::render()` が `SocialFonts::update()` に渡す値）も `min(高さ / 1080, 幅 / 1920)` を使っている。仕様の通り高さだけにして、1920×1080、1440×1080、1280×1024、2560×1080 で崩れないかを見る。崩れる場合だけ今の方式を残し、スクリーンショットを付けて報告する。
2. ウィンドウのサイズを変えている間、新しいブラーができるまで背景が単色に戻る（`SocialBackground::draw()`）。ヘッダーのコメントにある通り、新しい画像ができるまでは前の画像を引き伸ばして描く。
3. 描画スレッドの待ち：`wait_for(0)` を確かめずに `get()` しているところ、フレームの途中で実行中の `std::async` の future を破棄するところがないか。終了時（`MediconStopOverlay`）に、ブラーやアトラス作りの完了を待つ時間がどれくらいかを測って報告する。
4. ログ：キー、トークン、お知らせの本文がログに出ていないか。Preview のシナリオと E2E スクリプトも含めて `grep` で確かめる。
5. `social.json` の書き込みは送信スレッドだけで行い、描画スレッドでは書かない。
6. ログイン欄：Ctrl+V で貼り付けられる。Enter でログインする。
7. 閉じ方：Esc で閉じ、パネルの外のクリックでは閉じない。パネルの「×」は仕様にない追加なので、報告に書く。
8. サイドバー下の接続状態：仕様の表示は「オンライン／再接続中／オフライン」。今は「接続中…」「別の場所で接続中」もある。そのままでよいが報告に書く。
9. `social_changelog` シナリオは、タブの位置を決め打ちの座標でクリックしている。レイアウトの計算を使うか、§4.3 で足すタブ指定の関数を使う。

## 6. ビルドとテスト

`D:\code\Medicon` で、同梱のツールを使う。

```powershell
.\.tools\python\cmake\data\bin\cmake.exe -S . -B out\build\local
.\.tools\python\cmake\data\bin\cmake.exe --build out\build\local --parallel 6
.\.tools\python\cmake\data\bin\ctest.exe --test-dir out\build\local --output-on-failure
```

- Windows PowerShell 5.1 では、これらに `2>&1` や `*>&1` を付けない。CMake は stderr にも書くので、PowerShell がエラーとして扱う。ログをファイルに残したいときは Git Bash で動かす。
- `ctest` 全体で約3分かかる（`overlay_*` は1件20秒ほど）。
- Preview の例：`.\out\build\local\bin\MediconPreview.exe --scenario social_home --size 1920x1080 --fixed-dt 0.016666667 --screenshot C:\temp\social_home.png`
- Preview の主なオプション：`--scenario`、`--size`、`--fixed-dt`、`--screenshot`、`--main-menu`、`--main-menu-time`、`--social-dir`、`--gpu-timing`、`--gpu-frames`。
- 新しく足したテストやシナリオは `CMakeLists.txt` の `add_test` に登録する。

## 7. E2E 用の使い捨てサーバー

`social-server` には `.env` がない。本物のデータ（`social-server\data`）を作らないよう、ポート、DB、バックアップ先、管理用の秘密の値を環境変数で与える。環境変数は `.env` より優先される。

```powershell
cd D:\code\Medicon\social-server
$env:Path = "$PWD\.tools\node-v24.22.1-win-x64;$env:Path"
$work = Join-Path $env:TEMP "medicon-social-e2e"
New-Item -ItemType Directory -Force $work | Out-Null
$env:PORT = "18787"
$env:ADMIN_PORT = "18788"
$env:DB_PATH = "$work\social.db"
$env:BACKUP_DIR = "$work\backups"
$env:ADMIN_SECRET = "<16文字以上のランダムな値>"
node src\index.ts
```

- `listening on http://127.0.0.1:18787` と出れば起動している。
- 同じ環境変数を設定した別のシェルで、管理 CLI を使う。`npm` が動かなければ `npm.cmd` を使う。
  - キーの発行：`node admin\cli.ts license:create --name codex-e2e`
  - お知らせ：`node admin\cli.ts news:post --kind news --title "タイトル" --body-file <本文のファイル>`
  - Change Log：`node admin\cli.ts news:post --kind changelog --version v0.3.0 --title "タイトル" --body-file <本文のファイル>`
  - そのほか：`license:revoke --name`、`user:ban --name`、`user:unban --name`、`license:list`
- Preview の Social フォルダ（`--social-dir`）には、`{"serverUrl":"http://127.0.0.1:18787"}` だけの `social.json` を置く。
- 終わったらサーバーを止め、`$work` と Preview の Social フォルダを消す。

## 8. 完了の判定

- [ ] ログイン画面でキーを登録すると、ホームにお知らせと Change Log が出る（§4.6。ゲーム内の手順も報告に書く）
- [ ] 開閉アニメーションが動く（§4.7）
- [ ] 画面を開いても FPS が大きく落ちない（§4.8）
- [ ] Social ボタン、背景、パネル、サイドバー、ログイン、ホーム、設定が仕様 4.6 の通り（§4.1〜§4.5、§5）
- [ ] `debugLicenseKey` が残っていない
- [ ] ビルドが成功し、`ctest` がすべて成功する（新しいテストを含む）

## 9. 報告の形

- 変更したファイルの一覧（新規、変更、削除）。それぞれに1行の説明。
- 動作確認の手順（Preview のコマンド、E2E スクリプト、ユーザーがゲーム内で行う手順）と結果（スクリーンショット、FPS の数値）。
- 未解決の点と `TODO(social)` の一覧。少なくとも次を含める：ポーズ画面と他のバニラの画面を「ゲーム中」と区別できないこと、IME の変換中の Esc（フェーズ4）、バッジのデータ（フェーズ5・6）、DirectWrite とカラー絵文字を使っていないこと。
- 追加した Hook（ないはず）と、追加した外部ライブラリ（ないはず。D2D と WIC は Windows SDK）。
- 仕様と違う判断をした点と、その理由。
- キーやトークンは報告に書かない。

### ゲーム内でユーザーに確かめてもらう手順（報告に書く内容の例）

1. `social-server` を起動し、キーを発行する。お知らせと Change Log を投稿しておく。
2. `%APPDATA%\Medicon\social.json` の `serverUrl` をサーバーの URL にする。
3. Minecraft に DLL を読み込み、メインメニュー下中央の Social ボタンを押す。ログイン画面でキーを貼り付けてログインし、ホームにお知らせと Change Log が出ることを確かめる。
4. 開閉のアニメーションと Esc で閉じることを確かめる。設定を変えて `social.json` に反映されることを確かめる。ログアウトするとログイン画面に戻ることを確かめる。
5. ゲーム中に ClickGUI を開き、通知をクリックすると Social 画面が開くこと、その間ゲームが操作を受けないことを確かめる。
6. `debugTiming` を `true` にして、Social を閉じているときと開いているときの Perf 行の `fps` と `social` を比べる。
