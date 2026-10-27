# Social

Medicon Social のクライアント側です。ログイン、ホームのお知らせ・Change Log、Global チャット、フレンド、DM、通知設定と通知ポップアップを扱います（実装指示書のフェーズ1〜6）。ゲームと Preview は共通の Overlay と Social 描画コードを使います。メッセージの削除と BAN の反映（フェーズ7）はまだです。

画面の文字はすべて英語です。サーバーから届く本文（お知らせ、名前）は日本語でも表示できます。見た目はメインメニューだけを基準にしています（下の「見た目」）。

## 構成

| 場所 | 中身 | 動くスレッド |
|---|---|---|
| `medicon_social_core`（`src/social/Protocol.cpp`、`Config.cpp`、`Store.cpp`） | 通信の形式、`social.json` の解釈、SocialStore、スレッド間キュー、再接続の待ち時間 | どこでも（WinHTTP・ImGui に依存しない） |
| `medicon_social_net`（`SocialNet.cpp`、`SocialApi.cpp`、`WinHttp.cpp`、`LicenseStore.cpp`、`Service.cpp`、`Sound.cpp`） | WinHTTP の REST と WebSocket、DPAPI での鍵の保存、通知音 | ネット側 |
| `medicon_gui`（`SocialUi.cpp`、`SocialScreen.cpp`、`SocialBackground.cpp`、`SocialFonts.cpp`、`SocialWidgets.cpp`、`Toasts.cpp`） | イベントの反映、画面管理と描画、通知ポップアップ | 描画スレッドのみ（画像・フォント・影の準備はワーカー） |

スレッドは3本です。

- 接続スレッド：`social.json` と `license.bin` を読み、サインイン → WebSocket 接続 → `WinHttpWebSocketReceive` で待機します。受信した JSON はここで解析し、イベントにしてキューに積みます。
- 送信スレッド：条件変数で待ち、30秒ごとに `{"t":"ping"}` を送ります。
- REST ワーカー：UI からの依頼（今はお知らせ一覧の取得）を1件ずつ処理します。

描画スレッドは毎フレームの最初（フォント準備の前）にキューを確認します。空なら atomic フラグを1回読むだけで戻り、通知がなく画面も閉じていてメニューでもなければ描画もしません。ファイル I/O、通信、JSON 解析、待機は描画スレッドでは行いません。

## 起動と停止

- DLL は DXGI フックの設置が終わった後に Social を開始します。DllMain からは開始しません。
- `MediconStopOverlay()` では、close フレームを送ってサーバーの応答を待ち、スレッドを最大2秒で join します。終わらないスレッドは切り離します（スレッドが使うオブジェクトは自分で保持しているので解放後に触れることはありません）。
- ゲームを終了したときは、プロセスの終了と同時に OS がソケットを閉じ、サーバー側は即座に切断として扱います。ウィンドウの破棄は終了の合図として確実ではないため（作り直される可能性がある）、ここでは明示的な close をしません。

接続状態は `NoLicense → Authenticating → Connecting → Online` と進みます。通信の失敗は `Reconnecting`（1, 2, 4, 8, 16, 30秒、±20%）、キーの拒否・revoke・BAN は `AuthFailed`、別の場所での接続（close 4009）は `Replaced` になり、後の2つは再接続しません。close 4001 はトークンを取り直してすぐに再接続します。75秒何も受信しなければ切れたものとして再接続します。

## 設定ファイル

`%APPDATA%\Medicon\social.json`。なければ既定値で作ります。JSON として壊れている場合は書き換えず、そのセッションは既定値で動きます。型が違う項目はその項目だけ既定値になり、`inject.log` に項目名を記録します（値は記録しません）。書き込みはネット側のスレッドだけが行います。

接続先はクライアントに組み込んだ `https://medicon.auau1000.com`（WebSocket は `wss://medicon.auau1000.com/ws`）です。`social.json` に URL は保存せず、旧 `serverUrl` が残っていても使いません。有効な設定ファイルの読み込み時または設定保存時に旧項目を削除します。

| 項目 | 既定値 | 内容 |
|---|---|---|
| `notify.dm` / `notify.friend` / `notify.news` | `true` | 種類ごとの通知の ON/OFF |
| `notify.inGame` | `true` | ゲーム中にも通知を出すか |
| `notify.sound` | `true` | 通知音 |
| `notify.inGameOffsetY` | `0` | ゲーム中の通知の Y 位置オフセット（1080p 基準の px、±1080 まで） |
| `debugTiming` | `false` | `inject.log` の Perf 行（2秒ごと）に、Social の処理時間 `social n=… avg=…us max=…us` を追加します |

`license.bin` は同じフォルダに置き、DPAPI（現在の Windows ユーザー）で暗号化します。キーは画面から入力します。旧 `debugLicenseKey` は認証には使われず、有効な設定ファイルの読み込み時または設定保存時に削除されます。

テストやプレビューでは、環境変数 `MEDICON_SOCIAL_DIR` で `social.json` と `license.bin` の場所を変えられます。

## 画面

### 開き方

- メインメニューの下端中央の **Social** ボタン（1080p で 220×58 px、左の列のボタンより小さい）。ガラスの見た目、影、押し方（押して離す、直前に押していたクリックは無視）は Play / Settings / Quit と同じ描画コードで、左端のアクセントの線だけありません。Social がつながっていないとき（DLL が Social を開始していないとき）は出しません。
- 右上の小さなバッジ（`theme::social::alert`）は、未読の DM か未処理のフレンド申請があるときに出ます（`SocialStore::attention()`）。
- メニュー中に通知をクリックすると、その内容の画面が開きます。お知らせは Home の News タブ、Change Log は Change Log タブ、DM はその相手との DM、フレンド申請はフレンド画面、接続についての通知は Home です。

### 起動時のサインイン（DLL）

- inject時のアニメーションが終わった後、キーが無い（`NoLicense`）かサーバーに拒否された（`AuthFailed`）ときは、サインイン画面を自動で出します。
- この画面は Esc でも × でも閉じられません。キーが受理される（`LoginFinished` が成功）と閉じ、下のメインメニュー（ワールド内で inject したときはゲーム）に戻ります。
- 保存済みのキーで認証中のとき、サーバーにつながらず再接続待ちのときは出しません。
- 後でサインアウトしたり、キーが revoke・BAN されたりしたときも、同じ画面がまた出ます。
- サインインするたびに（キーを入力したときも、保存済みキーで自動サインインしたときも）、`Medicon Social` の通知で `Welcome to {名前}` を出します。再接続では出しません。
- Preview のシナリオでは既定で無効です（`SocialUi::set_startup_sign_in`）。`social_sign_in_gate` で確認します。

### 開いている間

- 閉じるのは Esc か右上の ×（× は仕様にない追加です）。パネルの外をクリックしても閉じません。
- キー入力は Social が先に受け取り、ゲームとモジュールのホットキーには押下を渡しません（離した入力だけ渡すので、キーが押しっぱなしになりません）。
- パネルの下にある UI（メインメニュー、HUD、ClickGUI）は、画面が見えている間はポインターを受け取りません。開き切った後は描画もしません。閉じると、メインメニューはボタンを離した状態を1度見てからクリックを受け付けます。

### 見た目

メインメニューと同じアプリに見えるよう、メインメニューの描画をそのまま使っています。

- **フォント**：メインメニューと同じ同梱の DynaPuff。日本語は FontManager と同じく同梱の kawai.otf で補います（DynaPuff にある字は DynaPuff が優先）。Social 専用のアトラスにワーカーで作るのは前と同じです。大きさは Display 38 / Heading 21 / Title 16 / Body 14 / Small 12 px（1080p）。
- **色**：水色 `#7FCBFF`・白・淡いピンク `#FFD1E3` が基本です。灰色の文字、灰色のパネル、黒い影は使いません。文字はくすんだ青（本文 `#5884AE`、見出しやラベル `#5FAEE8`、補足 `#7D9EC2`、サインインの説明 `#6A8FB7`）、エラーは柔らかいピンク `#EE86B0`。値はすべて `Theme.hpp` の `theme::social` と `theme::glass` にあります。
- **ガラスの板**：メインメニューのボタンの描画（下にずらした淡い青、半透明の白、白い縁、左の細いアクセント線。ホバーで線がピンク）を `Glass.hpp` の `draw_glass_plate` に切り出し、メインメニューと Social の両方がこれを使います。ボタン、選択中のサイドバー項目、タブ、お知らせのカード、ホバー中の行やアイコンがこの板です。
- **影と光**：既存の Shadow（`MeshShadowRenderer`）に色を付けられるようにし（`ShadowParams::tint`）、黒ではなく青い影と、ずらさない淡い水色の光（Glow）に使います。
- **パネル**：ミルキーな水色 `#E7F4FF`（不透明度 74%）、角丸 20 px、白 70% の 1 px の縁、広く柔らかい青い影と、ごく薄い水色の光。ホームは 1000×620、サインインは 480×400（余白 44）の大きさで、`scale = min(高さ/1080, 幅/1920)` を掛けます。窓の 92% を超えません。
- **入力欄**：半透明の白に薄い水色の縁。フォーカスで縁が水色になり、柔らかい水色の光が 160 ms でふわっと出ます。
- **ボタン**：メインメニューの Play と同じガラスの板（白っぽい水色、左に水色の線）。ホバーで明るくなり、光が強まり、線がピンクに変わります（約 60 ms でなめらかに）。押している間は 0.98 倍に縮みます。
- **タイトル**：「Medicon Social」は DynaPuff の水色に、メインメニューのロゴと同じく周りに縁（白）を付けています。DynaPuff は同梱の太さが1つ（Medium）だけなので、階層は大きさと色で付けています。
- **開閉**：開くとき 220 ms で不透明度 0 → 1、0.96 → 1 倍（ease-out cubic）、閉じるとき 140 ms。影と光もパネルと一緒にフェードと拡大をします。

### 背景

暗い幕や灰色の背景は使いません。下から順に次の3つを重ねます。

1. メインメニューのパステルのグラデーション（`draw_main_menu_backdrop`）。メインメニューと同じ時刻で描くので、メニューから開くとグラデーションがそのまま続きます。
2. `assets/back.png` をぼかしたもの（不透明度 38%）。DLL に埋め込み（既存の `mez.png` やフォントと同じ `cmake/EmbedFonts.cmake`）、ワーカースレッドで WIC でデコードし、縦横比を保って画面を覆うよう中央でクロップして、WIC オフスクリーンターゲットの D2D 1.1 GaussianBlur（σ = 8 × 高さ/1080、HARD）を掛けます。ぼかした画像は 1/3 程度に縮めて作り、GPU で引き伸ばします。D2D が使えないときは CPU の Gaussian フィルターです。
3. ミルキーな白い幕（`#F4FAFF`、不透明度 16%）。

パネルの中でも、サイドバーより右（ホーム、設定などの内容の部分）は、同じ back.png を軽くぼかしたもの（σ = 2.5 × scale）で埋め、ごく薄い白の幕（`#F6FBFF`、12%）を重ねています。右側の角だけパネルと同じ角丸です。デコードした画像は背景と共有し、内容の部分の大きさで別にワーカーでぼかします（1080p で 800×620、約 2 MB）。サインイン画面にはありません。

- 窓の大きさが変わったら画像を作り直します。その間は前の画像を引き伸ばして表示します。
- デバイス再生成時は保持した画像から再アップロードします。デコードに失敗したときはグラデーションと幕だけになります（パネルの中は水色のまま）。
- **DirectWrite とカラー絵文字による描画は未対応です。** 既存の ImGui/D3D 描画を使います。

### 先読み

メインメニューか ClickGUI が出ている間、Social のフォント（画面の固定文字列）、ぼかした背景、パネルの影と光、サインインの入力欄とボタンの光のメッシュをワーカースレッドで用意します。初めて開いたフレームから文字と背景がそろって出ます。Preview の `social_first_open` で、メニューに入ってから約 130 ms で準備が終わり、開いた最初のフレームの CPU 時間は約 1.3 ms でした（影を先に作らない場合は約 13 ms）。

使うメモリは、デコードした back.png（1672×941 RGBA、約 6.3 MB、窓の大きさが変わったときの作り直し用）、ぼかした画像の CPU 側の控え（1080p で 640×360、約 0.9 MB）と同じ大きさの GPU テクスチャ、Social 専用のフォントアトラスです。

### ログイン、ホーム、設定

ログイン画面は `Login` コマンドを送信スレッドに渡し、`SocialNet` が認証します。成功後に `LicenseStore` で暗号化保存し、結果と接続イベントを返します。ホームは `FetchNews` を REST ワーカーへ送り、`SocialApi` が取得・解析した `NewsPage`（20件ずつ）をイベントとして返します。一番下近くまでスクロールすると続き（`before` = 最後の ID）を取得します。設定変更とログアウトもコマンド経由で処理します。

| エラー | 表示 |
|---|---|
| `invalid_key` | That license key is not valid |
| `revoked` | This license key has been revoked |
| `banned` | This account is suspended |
| `unauthorized` | Could not sign in. Check your key |
| `rate_limited` | Too many attempts. Try again later |
| `bad_server_url` | This client's server URL is invalid |
| 通信できない | Cannot reach the server |

### 入力欄

`SocialTextInput` が Social の入力欄の共通部品です（今はライセンスキー欄。Global と DM の入力欄もこれを使います）。

- 文字はグラフェムクラスタ（サロゲートペア、結合文字、異体字セレクタ、肌の色、ZWJ でつないだ絵文字、国旗）単位で扱い、カーソル移動と削除もその単位です。
- **入力アニメーション**：入力した文字だけが、少しグレー（`#8A8A8A` へ 30%、不透明度 0.85）で始まり、60 ms で文字サイズ×0.12（1.5〜3 px）沈み、120 ms で元の位置と色に戻ります（計 180 ms、行き過ぎなし）。時間は QueryPerformanceCounter の実時間で、FPS に依存しません。
  - 文字ごとに出現時刻を持ち（テキストと同じ長さの配列）、挿入・削除で一緒に増減するので、途中に入れた文字だけが動き、前後は動きません。新しい入力で前の文字のアニメーションは止まりません。
  - 貼り付けは先頭24文字を 12 ms ずつずらして動かし、25文字目以降はすぐ表示します。同時に動くのは64文字までです。
  - 削除、カーソル移動、選択では動きません。選択範囲を上書きしたときは新しい文字だけが動きます。
  - キャレットは常に最終位置に描きます。
  - 動き終わった後の描画は、アニメーションなしのときと同じ頂点になります（テストで確認）。
  - ON/OFF は ClickGUI の **ClickGUI → Input Animation**（既定 ON）。OFF のときは従来の描画だけです。
  - 64文字が同時に動いても描画の増加は約 6 µs です（`medicon_social_ui_tests`）。
  - 日本語 IME：変換中の文字列は動かさず、確定した文字列が入力として動きます（下の「入力と IME」）。
  - ClickGUI の設定エディタにある文字列設定の入力欄は ImGui 標準の `InputText` で、1文字ずつ描き分けられないため対象外です。

## Global、フレンド、DM

### Global（フェーズ4）

- サイドバーの Global で開きます。上にメッセージ一覧、下に入力欄と Send。Enter でも送れます。空なら何もしません。
- 1件の表示は名前（自分は水色）、時刻 HH:MM、本文（折り返し）。同じ人が5分以内に続けて書いたものは名前を省いてまとめます。
- 一番下を見ているときだけ新着で下へ流れます。上を読んでいるときは「New messages ↓」ボタンが出ます。
- 一番上までスクロールすると過去分（50件ずつ）を読み込み、見ている位置は動きません。
- 自分の送信はサーバーの確認（ack）まで不透明度 50% で出ます。断られたら「Failed to send · Click to retry」（原因によって Sending too fast など）になり、クリックで再送します。
- rate_limited などの理由は入力欄の上に2秒だけ出ます（Global は「One message every 0.3 seconds」）。
- 本文は300コードポイントまで。250を超えると残りの文字数が出ます。

### フレンド（フェーズ5）

- サイドバーの「DM」の右の + でフレンド画面を開きます。未処理の申請があると + にバッジが付きます。
- 検索欄は入力が止まって 300 ms 後に `GET /users/search` を呼びます。結果の行には関係に応じたボタン（Add / Requested / Message / Accept）が出ます。
- REQUESTS（届いた申請）は Accept と Decline、SENT（送った申請）は Cancel。エラーは検索欄の下に3秒出ます（No user with that name、Request already sent、Too many requests など）。
- サイドバーのフレンド一覧はオンラインの点（緑か灰）、名前、未読数のバッジ。クリックで DM を開きます。右クリックで「Remove friend」が出て、確認（Remove / Cancel）の後に削除します。Esc はまずメニューや確認を閉じます。
- 申請、承認、削除の結果はサーバーからの通知（friend.request / friend.update / presence）で反映し、自分の操作の後は一覧を取り直します。

### DM（フェーズ6）

- 一覧は Global と共通の部品です。自分のメッセージは右寄せの水色の吹き出し、相手は左寄せの白。ヘッダーに相手の名前とオンライン状態。
- 開いたとき、および一番下で新着を表示したときに `dm.read` を送り、未読を0にします。
- 相手が既読にした自分の最後のメッセージの下に「Read」が出ます。
- 再接続のたびに `GET /friends` と、開いている Global・DM の最新50件を取り直し、ID で重複を除いて統合します。

### 入力と IME

- 文字は WM_CHAR をキーと同じ順に直接 Social の入力欄へ渡します（ImGui の入力キューは、直前のホイールなどの後ろで1フレーム以上遅れることがあり、Enter より後に届いて送信から漏れました）。
- 日本語 IME は `ImeBridge`（`src/gui/Ime.cpp`）が受け持ちます。Social の入力欄にフォーカスがある間だけ、ゲームのウィンドウで IME を有効にし（`ImmAssociateContextEx(…, IACE_DEFAULT)`）、外れたら元に戻します。
  - 変換中の文字列（GCS_COMPSTR）は入力欄の中に下線付きで描きます。Windows の変換ウィンドウは出しません（WM_IME_SETCONTEXT で ISC_SHOWUICOMPOSITIONWINDOW を外す）。
  - 確定した文字列（GCS_RESULTSTR）は入力として入り、入力アニメーションで動きます。
  - 変換候補のウィンドウはキャレットの下に出します（ImmSetCompositionWindow / ImmSetCandidateWindow）。
  - 変換中の Esc は変換の取り消しに使い、画面は閉じません（既存の処理）。
  - ゲームのウィンドウプロシージャの中で、IME のメッセージだけを扱います。新しい Hook は追加していません。
- ひらがな・カタカナ・全角記号はフォントに先に入れてあるので、打った瞬間から表示されます。漢字は初めて使ったときにアトラスへ追加され、一瞬（約0.1秒）空白になります。

## ランクと Admin

- ランクは `user` と `owner`。owner にするにはサーバーで `npm run admin -- user:rank --name <name> --rank owner` を実行します（接続中ならその場で再接続して反映）。
- owner には Global の名前の横とサイドバー下部に OWNER タグが付きます。
- owner が Social を開くと Medicon の News / Change Log の右に Admin タブが出ます（user には出ません）。
  - 種類（News / Change Log）、TITLE、VERSION（Change Log のみ）、TEXT（複数行。Enter で改行）を入力し、右の PREVIEW で表示を確かめます。
  - Post を押すと確認（「Post this Change Log?」、取り消せない旨）が出て、そこで Post を押したときだけ送信します。Cancel か Esc で戻ります。
  - 成功すると「Posted」が出てフォームを空にし、全員の News / Change Log に届きます。失敗は理由（Only owners can post、A title and text are required など）を表示します。
- Change Log の本文で、行頭の `+` は緑、`-` は赤、`*` は黄色で表示します（例：`+ Added Killaura` / `- Removed antistaff` / `* Fixed auth`）。折り返した続きの行には付きません。

## 通知ポップアップ

- 画面上端の中央、上から 16 px。ゲーム中は `inGameOffsetY` を足します。サイズはすべて「ウィンドウの高さ / 1080」倍です。
- 白に近い水色（不透明度 88%）、角丸 16 px、白い縁、既存の Shadow を青くしたもの。文字は画面と同じ DynaPuff。左端の帯は DM が `#7FCBFF`、フレンドが `#FFD1E3`、お知らせが `#D9F2FF`、接続についての通知が淡いピンクです。
- 200 ms で上から 20 px スライドしながら表示し、メニュー中は4秒、ゲーム中は3秒後に 200 ms で消えます。メニュー中はカーソルを重ねると止まります。
- 同時に3件まで（8 px 間隔）。それ以上はキューに入れます。同じ相手の DM は1件にまとめて「+N」を出します。
- 開いている DM の相手からの DM は通知しません。DM とフレンド申請の通知は、設定の Direct messages / Friend requests に従います。
- クリックはメニュー中だけ有効で、上の「開き方」のとおり画面を開きます。Social 画面の上に重なった通知の下では、画面はポインターを受け取りません。
- 文字は Social 専用アトラスを画面と共有します。ClickGUI のアトラスとは独立しています。

### サーバーとの接続について出す通知

再接続しない理由も通知で表示します（`Medicon Social` という見出しで、ゲーム中表示の設定に従います）。

| 状態 | 表示 |
|---|---|
| キーが違う（401 `invalid_key`） | Invalid license key |
| revoke（403 または close 4003） | This license key has been revoked |
| BAN | This account is suspended |
| close 4009 | Signed in from another location |
| ビルドに組み込んだ URL が不正 | This client's server URL is invalid |

### ゲーム中とメニュー中の判定

既存の判定を使い、Medicon のメインメニューか ClickGUI が開いている間（Medicon がカーソルを描いている間）を「メニュー中」としています。ポーズ画面、設定画面、ワールド選択などのバニラの画面は区別できないため「ゲーム中」として扱います（`src/gui/Overlay.cpp` の `TODO(social)`）。

## ログ

`%LOCALAPPDATA%\Medicon\inject.log` に `Social:` で始まる行として、接続先、状態の変化、通信エラーのコード（HTTP の状態や WinHTTP のエラー番号）を記録します。ライセンスキー、トークン、お知らせの本文は記録しません。

## ビルドとテスト

```bash
.tools/python/cmake/data/bin/cmake.exe -S . -B out/build/local
.tools/python/cmake/data/bin/cmake.exe --build out/build/local --parallel 6
.tools/python/cmake/data/bin/ctest.exe --test-dir out/build/local --output-on-failure
```

Minecraft に `Medicon.dll` を読み込んだままだと、DLL のリンクだけが失敗します（ファイルが使用中）。ゲームを閉じてからビルドしてください。

## Preview で確かめる

Minecraft なしで確かめられます。`--size 1920x1080 --screenshot out.png` を付けると描画を保存します（複数枚のシナリオは名前の後ろに段階が付きます）。モックのシナリオはネットワークも実際のキーも使いません。

| シナリオ | 内容 |
|---|---|
| `main_menu_social` / `main_menu_social_badge` | メインメニューの Social ボタン（バッジあり） |
| `main_menu_social_click` | ボタンのクリックで画面が開き、開き切るとメニューを描かない |
| `social_login` / `social_login_success` / `social_login_error` / `social_login_network` | サインイン画面 |
| `social_home` / `social_changelog` / `social_empty` / `social_many` / `social_settings` | ホームと設定 |
| `toasts` / `toasts_menu` / `toasts_click` | 通知（クリックで Change Log タブが開く） |
| `social_global` | Global：名前と時刻のまとまり、日本語、送信中（50%）、失敗と再送、入力欄の上のエラー |
| `social_dm` | DM：吹き出し、Read、サイドバーのオンラインの点・未読バッジ・+ のバッジ |
| `social_friends` | フレンド画面：300 ms 後の検索、届いた申請、送った申請 |
| `social_friend_remove` | 右クリックの「Remove friend」と確認 |
| `social_admin` | owner の Admin タブで Change Log を書き、警告を経て投稿（`PostNews` が確認後にだけ出る） |
| `social_changelog_marks` | Change Log の `+` `-` `*` の色分け |
| `social_sign_in_gate` | 起動時のサインイン：閉じられないこと、受理後にメインメニューへ戻ること、Welcome 通知、再接続では出ないこと |
| `social_login_states` | サインインの入力欄のフォーカス、Sign in ボタンのホバーと押下 |
| `social_animation` | 開く 0/70/140/220 ms、閉じる 0/50/90/140 ms（`--fixed-dt 0.01` で時刻ちょうど） |
| `social_typing` | 入力アニメーション 0/30/60/120/180/300 ms と、秒間15文字の連打（`--fixed-dt 0.01`） |
| `social_first_open` | メニューで先読みした後、開いた最初のフレームの時間 |
| `social_device_recreate` | リソース生成後のデバイス再生成 |
| `social_scroll` / `social_toasts_steady` / `main_menu_idle` | FPS 計測用（スクロールし続ける、通知3件を出し続ける、Social なしのメニュー） |

### 実サーバーでの確認

ローカル E2E には、通常の配布用 DLL と別のビルドディレクトリで CMake に `-DMEDICON_SOCIAL_SERVER_URL=http://127.0.0.1:18787` を指定して作った Preview を使います。接続先はビルド時に固定され、設定ファイルや実行時の環境変数では変更できません。

```bash
social-server/.tools/node-v24.22.1-win-x64/node.exe tools/social-e2e.mjs --preview out/build/social-e2e/bin/MediconPreview.exe
```

使い捨てのサーバー（ポート 18787/18788、一時フォルダの DB）を立て、Preview を2回動かします。キーは環境変数 `MEDICON_SOCIAL_TEST_KEY` で Preview に渡し、どこにも表示しません。`%APPDATA%\Medicon` には触れません。

1. サーバー停止中にサインイン（Cannot reach the server）→ サーバー起動 → 知らないキー（That license key is not valid）→ 本当のキーで Online。`license.bin` ができ、キーが平文で入っていないこと。
2. Home：25件のお知らせが新しい順に出て、スクロールで20件目より先も読み込むこと。Change Log タブ。
3. 設定で Sound を OFF → `social.json` に保存されること。Sign out で `license.bin` が消えること。もう一度サインイン。
4. 2回目の起動：`license.bin` だけで Online。news:post で通知、サーバー再起動で再接続、revoke で This license key has been revoked。
5. すべてのログにキーとお知らせの本文が出ていないこと。

Global・フレンド・DM（フェーズ4〜6）は別のスクリプトで確かめます。

```bash
social-server/.tools/node-v24.22.1-win-x64/node.exe tools/social-chat-e2e.mjs
```

Preview が1人（dll）、スクリプトが bob・cat・eve として WebSocket で参加し、次をすべて確かめます。

1. Global：上へのスクロールで古い60件を読み込む。DLL で打った日本語が bob に届き、bob の日本語が DLL に出る。連投は「One message every 0.3 seconds」になる。
2. フレンド：bob の申請が通知とフレンド画面に出て、Accept で bob に friend.update accepted が届く。cat への申請と cat からの申請が交差するとその場でフレンドになる。
3. DM：bob の DM 2件で未読バッジ、通知、メインメニューのバッジ。開くと bob に dm.read が届く。DLL の返信を bob が読むと Read が出る。開いている DM の相手からの新着は通知しない。フレンドでない eve の DM と履歴は拒否される。
4. ログにキーと本文が出ていない。

どちらのスクリプトも、サーバーの URL を `http://127.0.0.1:18787` にした別の Preview（既定は `out/build/e2e`。`-DMEDICON_SOCIAL_SERVER_URL=http://127.0.0.1:18787` と、メインと同じ `-static` のリンク設定で作る）を使います。

`--scenario social_live` / `social_live_login` / `social_live_chat` を直接使うときは `--social-dir <フォルダ>` を付けます。

### FPS

`--gpu-timing --gpu-frames 600 --size 1920x1080` で、60フレームのウォームアップの後に 600 フレームの GPU 時間（UI の描画パス）と CPU 時間を測ります。Minecraft 本体での計測の代わりにはなりません。2026-10-27（RTX 3060、メインメニューに合わせた見た目にした後）の結果：

| 場面 | GPU 平均 / p95 / 最大 (ms) | CPU 平均 / p95 / 最大 (ms) |
|---|---|---|
| Social なしのメインメニュー（`main_menu_idle`） | 0.075 / 0.082 / 14.4 | 0.039 / 0.089 / 0.25 |
| Social ボタンと先読みあり（`main_menu_social`） | 0.054 / 0.087 / 0.57 | 0.040 / 0.093 / 0.35 |
| ホーム（`social_home`、パネルの中の画像あり） | 0.176 / 0.192 / 0.93 | 0.213 / 0.373 / 0.61 |
| スクロール中（`social_scroll`） | 0.183 / 0.178 / 14.1 | 0.200 / 0.308 / 0.79 |
| 通知3件（`social_toasts_steady`） | 0.088 / 0.088 / 14.0 | 0.144 / 0.215 / 0.47 |

平均の増加は最大でも CPU 0.17 ms、GPU 0.11 ms です（目標は 0.5 ms 以下）。GPU の最大値の 14 ms 前後は Social なしのメニューでも出る単発の値で、Social の描画とは関係ありません。

ゲーム内では `social.json` の `debugTiming` を `true` にすると、`inject.log` の Perf 行に `fps` と Social の処理時間が出ます。同じワールド・同じ場所・同じ描画設定で、Social を閉じて通知のない状態、ホームを開いた状態、ホームをスクロールしている状態、通知が出ている状態を、それぞれ10秒以上比べてください。

## ゲーム内での確認手順

1. `social-server` を起動し、`npm run admin -- license:create --name <name>` でキーを発行する。
2. Cloudflare Tunnel で `medicon.auau1000.com` をサーバーの `http://127.0.0.1:8787` に転送する。クライアント側で接続先を設定する必要はない。
3. Minecraft に DLL を読み込み、`inject.log` に `Social:` の行が出ることを確認する。
4. inject のアニメーションの後に出るサインイン画面で、発行したキーを入力して Sign in。メインメニューに戻り、`Welcome to {名前}` の通知が出ることを確認する。Social ボタンで画面を開き、ホームにお知らせと Change Log が出ることを確認する。
5. `npm run admin -- news:post ...` で、メインメニューとゲーム中のそれぞれで通知が出ること、メニュー中に通知をクリックすると該当するタブが開くことを確認する。
6. サーバーを止めて起動し直し、`Social: reconnecting` → `Social: online` になることを確認する。
7. `debugTiming` を `true` にして、上の FPS の比較をする。
