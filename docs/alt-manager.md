# AltManager

Main MenuのSocialボタンの右隣から開く、Microsoftアカウント管理画面。
Microsoft連携・保存・削除に加え、`Use` で選んだアカウントを次回のサーバーログインに使う。
Minecraft 1.26.52.03 の対象PEとsigが一致した場合に有効。接続中なら一度抜けて入り直す。
認証完了時は `Selected`、そのアカウントのトークンを接続要求に使うと行が緑色の `In use` になる。

## 構成

| 部分 | 場所 | 内容 |
| --- | --- | --- |
| Main Menuボタン | `src/gui/MainMenu.cpp` | Socialは中央のまま、右隣に配置。alt serviceが接続されている時だけ表示 |
| 画面 | `src/gui/AltScreen.cpp` | Socialのパネルを流用し、サイドバーを右へ反転。一覧が中央、右にMicrosoftボタン |
| 認証 | `src/alt/Auth.cpp` | デバイスコードフローとXbox Live。通信とウィンドウは `SignInHost` 越しなので偽物で試験できる |
| 接続認証 | `src/alt/Session.cpp` | refresh → Xbox → PlayFab → Minecraft session / multiplayer token。返された `xid` とゲームの公開鍵との一致を確認 |
| ゲーム接続 | `src/platform/AutoOverlay.cpp` / `src/sdk/BedrockAuth.cpp` | native HTTPで接続トークンを取得する前に選択セッションへ切り替え。`ConnectionAuthInfo::toJson` では同じnative Tokenを検証して通す |
| Worker | `src/alt/AltService.cpp` | `SocialService` と一緒に起動・停止。`alts.bin` の管理とログイン実行 |

## ログイン

Microsoftボタンで、Microsoft自身のログインページを開く。パスワードはそのページにしか入力されず、
Medicon（注入先プロセス）を通らない。

1. `login.live.com/oauth20_connect.srf` からデバイスコードを取得（scope `XboxLive.signin offline_access`）。
   コード取得とtoken取得には同じMSAエンドポイントを使う
2. Microsoftが返した `verification_uri` を変更せず、EdgeのInPrivateウィンドウで開く。
   起動ごとに新規作成した空のプロファイルを `--user-data-dir` に指定し、既存の通常・InPrivateセッションともCookieを共有しない。
   新しいアカウントの追加と `Open Microsoft` での開き直しは、毎回Cookieのない状態から始まる。
   Edgeの起動に失敗した場合は通常ブラウザへ引き継がず、表示されたコードと再試行操作を残す。
   ブラウザを開く前にコードをクリップボードへ自動コピーし、Alt Managerの中央にも大きく表示する。
   Microsoftの画面に `Ctrl+V` で貼り付けてログインする。
   `Copy code` / `Copy again` で再コピー、`Open Microsoft` で認証ページを開き直せる。
   コピーに失敗した場合は成功表示にせず、コードの手入力と再コピーを案内する。
   ウィンドウ終了時はその起動用プロファイルだけを削除する。別のログイン中のプロファイルには触れない
3. `login.live.com/oauth20_token.srf` をポーリング。Cancelか有効期限で中止。
   ブラウザのプロセス終了では取り消さない（ブラウザの別プロセスへの引き継ぎや、別端末での認証を許す）
4. `user.auth.xboxlive.com` → `xsts.auth.xboxlive.com`（RP `http://xboxlive.com`、contract version `1`）でXboxサービス用tokenを取得。
   `xid` / `gtg` が揃わない場合は、`profile.xboxlive.com/users/me/profile/settings?settings=Gamertag` を
   `Authorization: XBL3.0 x=<uhs>;<XSTS token>`、contract version `2` で取得し、`profileUsers[].id` と `Gamertag` を使う。
   tokenにXUIDがある場合はプロフィール応答のIDと一致することを確認する
5. `%APPDATA%\Medicon\alts.bin` にDPAPI（CurrentUser）で暗号化して保存。tokenはUIへ渡さない

### Microsoftの画面が開いた直後に `first party application ... invalid_request` が出る場合

以前はコードを自動入力するため `verification_uri` に `?otc=<code>` を付けていた。
このURL形式で同じエラーになる[報告](https://learn.microsoft.com/zh-cn/answers/questions/2137976/otc-url)があるため、
自動入力パラメータを追加せず、表示したコードを入力する方式へ変更した。
[Microsoftのデバイスコードフロー仕様](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code#authenticating-the-user)も、
`verification_uri` と `user_code` を案内する方式を定義している。エラー文だけでアプリ登録の問題と断定しない。

設定済みアプリIDでMSAのコード取得がHTTP 200になることは確認済み。
2026-10-31、修正後の実アカウントでMicrosoft認証からAlt一覧への追加までユーザー確認済み。

### Microsoft連携後もAlt一覧が空の場合の修正

2026-10-31の実ログでは、Microsoft token取得とXboxユーザー認証はHTTP 200、XSTS認証はHTTP 400だった。
表示情報を取るためRPを `https://xboxlive.com` にした変更では、このログイン経路が拒否された。
Xboxサービス向けの標準RP `http://xboxlive.com` に戻し、表示情報は必要に応じてプロフィールAPIから補完する。
[MicrosoftのRP対応表](https://learn.microsoft.com/en-us/gaming/gdk/docs/services/fundamentals/s2s-auth-calls/s2s-calls/live-title-service-calls-xbox-live)と
[Xbox Profile API実装](https://github.com/OpenXbox/xbox-webapi-python/blob/master/xbox/webapi/api/provider/profile/__init__.py)を参照。
認証・XSTS・プロフィールのどこで失敗したかを分け、HTTPと数値のXErrだけを画面とログへ渡す。
応答本文、token、サーバーが返した任意のMessageは診断に含めない。「プロフィール無し」は既知のXErrの場合だけ表示する。
また、ブラウザの終了を検知してtoken取得前に無言でキャンセルしていた処理を廃止した。

`tests/social_ui_tests.cpp` ではMicrosoftの応答だけを代替し、実際の認証処理 → worker → DPAPI保存 →
Alt一覧のイベント反映 → worker再起動後の復元まで検証する。
`tests/alt_clipboard_tests.cpp` は独立したWindows window stationで実際のコピー・読み戻しを検証し、
ユーザーのクリップボードは変更しない。Previewでも認証待ち・再コピー・キャンセル・連携後の行表示を検証する。

## 必要な設定: Microsoft EntraアプリのクライアントID

Minecraft本体のクライアントIDは埋め込んでいない。自分のアプリを登録してビルド時に渡す。
未設定のビルドでは通信もウィンドウ表示も行わず、「Microsoft sign-in isn't set up in this build」と表示する。

1. [Microsoft Entra管理センター](https://entra.microsoft.com/)の「アプリの登録」で新規登録。
   アカウントの種類に個人用Microsoftアカウントを含める
2. 「認証」で「パブリック クライアント フローを許可する」を有効化
3. 「アプリケーション（クライアント）ID」をビルドに渡す

```powershell
cmake -S . -B out/build/local -DMEDICON_MSA_CLIENT_ID=<クライアントID>
```

`tools/build.ps1` は既存のキャッシュを保持するので、一度設定すれば以降も有効。

## サーバーアカウントの切り替え

1. `Use` が保存済みrefresh tokenを自前のEntraアプリで更新する。回転したrefresh tokenも保存する。
2. Xbox user tokenから、PlayFab用RP `https://b980a380.minecraft.playfabapi.com/` のXSTSを取得する。
3. `20ca2.playfabapi.com/Client/LoginWithXbox` のSessionTicketをMinecraft `/api/v1.0/session/start` へ渡す。
4. ゲームの `HCHttpCallPerformAsync` の直前に、HTTPSの正確な `/api/v1.0/multiplayer/session/start` のAuthorizationだけを選択セッションに変える。
   公開鍵を含む元のリクエスト本文、async block、nativeレスポンス処理はそのままにする。資格情報ヘッダーはトレース禁止。
5. ゲーム自身が受け取ったTokenをNetherNetとLoginPacketが使う。LoginPacket段階では別Tokenを発行しない。
   `xid`・公開鍵の存在・issuer・audience・expiry・nbfを確認してnative Tokenをそのまま通す。
   古いアカウントのTokenは空Tokenで拒否し、`native_session_stale` と再接続案内を出す。native HTTPの失敗番号も画面へ渡す。
6. ゲームが保持する秘密鍵・認証オブジェクト・Xbox/XALのグローバル状態は書き換えない。
   スキンとバニラのアカウント表示は引き続きゲーム側のもの。サーバーへ示す本人性は選択したアカウントの正規署名付きTokenが決める。

ゲーム関数の特定はsigに加え、対象PE timestamp `0x6AB54E37`、SizeOfImage `0x12C02000`、関数RVA `0x46ADD0` の一致を要求する。
異なる版ではUseを無効化する。フックは既存のcallbackカウント・停止・解除経路へ登録済み。

以下は実装前に洗い出した調査項目。

1.26.52.03 の実ファイルを IDA MCP と照合した結果は [SDK 調査](alt-manager-sdk-validation.md) と
[sig 一覧](alt-manager-1.26.52.03-signatures.json) に記録した。生成関数の ABI、XAL/GDK 経路、User identity 配置は静的確認済み。
自前Entraアプリと保存済み実アカウントでGameServerTokenの発行、XUID・公開鍵の一致まで確認済み。
14:44〜14:45のゲーム実行ログで、Useの全認証段、nativeフック経由のmultiplayer token取得、
接続要求への選択アカウントTokenのシリアライズまで確認済み。Hive側の表示と入場成功はログだけでは断定しない。

### NetherNet / notauthenticatedへの修正（v2）

ユーザー実測ではv1のToken発行が成功しても、NetherNet接続エラーと `disconnectionscreen.notauthenticated` が出た。
v1はLoginPacketのJSON化時に別Tokenを発行しており、それ以前のNetherNet認証へ反映されなかった。
[MojangのNetherNet資料](https://github.com/Mojang/bedrock-protocol-docs/blob/main/additional_docs/NetherNetOnboardingGuide.md#5-identity-assertions)が示す通り、
NetherNetのidentity assertionにもGameServerTokenと公開鍵の結び付けが必要なため、二段階で異なるアカウントを使う構造を廃止した。

v2は上記4〜5のnative発行経路で統一する。フックは`libHttpClient.GDK.dll`の公開C ABIを使用し、
`HCHttpCallGetRequestUrl` / `HCHttpCallRequestSetHeader` / `HCHttpCallPerformAsync` / `HCHttpCallResponseGetStatusCode`を使う。
リクエスト所有権・async callbackは借用し、変更・置き換え・保持しない。すべてのフックを同じ停止・解除グループへ登録。
資格情報を第三者のsignaling URLや似たホスト名へ渡さないことをテストしている。

実アカウントと実際のゲーム用`libHttpClient.GDK.dll`を使い、ヘッダー差し替え、公開鍵を含む本文の保持、native非同期HTTP 200、
返されたXUID / 公開鍵の一致とnative Tokenを変更しないことを確認済み。
2026-10-31、v2で「両方消えて、選んだアカウントで入れた」とユーザー確認済み。
15:13の実行ログでもnative要求の選択セッションへの切り替え、HTTP 200、native Tokenによるログイン要求の生成を確認。
実行時の検証範囲は[検証記録](alt-manager-1.26.52.03-runtime-validation.json)に残す。

### ゲーム本体のXboxアカウントとの違い

ゲーム実行中の読み取り診断でも、ClientInstanceのUserImpl → Xbox identityには、保存済みAltと異なるXUIDが残っていた。
Xbox identity / gamertag / XUser handleが存在するため、AltManagerのXbox認証失敗と混同しない。
現在のUseはゲーム内のXbox identityを切り替えていない。Xboxプロフィール・フレンド等のアカウントが変わったことを示す機能ではない。
緑色は接続要求に使ったアカウントの印。ゲーム本体のXboxサインイン完了の印には使えない。

診断は`out/analysis/native_xbox_status.py`。対象PEのgetter命令列を確認してから読み取る。
ID・gamertag・tokenの値は出力せず、存在と保存済みAltへの一致のみを記録する。ゲームメモリへの書き込み・API呼び出しは行わない。
Xbox本体を同じアカウントへ切り替えるには、ゲームの正式なXboxサインインとユーザー切り替えの完了通知まで統合する必要がある。

| 必要なもの | 用途 |
| --- | --- |
| ログインパケット生成箇所のsigとABI（`ConnectionRequest` 生成、または `LoginPacket` 構築） | サーバーへ送る認証情報を差し替える唯一の必須地点 |
| ゲームが認証tokenを取得する経路（GDKの `XUser*` / XAL 経由か、独自経路か）と、その呼び出し箇所 | 上の差し替えに渡すtokenの種類と形式の確定 |
| 現在のユーザー情報のSDK（`Social::User` 相当の配置、gamertag / XUIDの読み先） | ゲーム内表示とログイン内容の整合 |
| スキン情報の出所 | アカウントごとのスキンをログインに含める場合 |
| Minecraft用tokenの発行条件 | XSTSの `RelyingParty`、および自前のEntraアプリでそのtokenを取得できるかの確認。不可ならこの方式では切り替えできない |

## 実環境での確認

- 実アカウントでのMicrosoftログインとAlt一覧への追加はユーザー確認済み（2026-10-31）。
- Edgeの2ウィンドウを同時に起動する実ブラウザ試験で、開始時のCookie無し、セッション間の非共有、終了後の一時プロファイル削除を確認。
- `out/alt-session-smoke.cpp` で保存済み実アカウントを使い、refresh / Xbox user / PlayFab XSTS / PlayFab login / Minecraft session / multiplayer tokenの全段HTTP 200を確認。トークンと本人IDは出力せず、公開鍵とXUIDの一致を検証した。
- `alt`、`bedrock_auth`、`social_ui`、`preview_alt_manager` のCTest成功。PreviewはUseコマンド、認証待ちの非緑表示、接続認証イベント後の緑表示を操作で確認する。
- エラー分岐とプロフィールAPI補完後のDPAPI保存・再起動復元は、外部サーバー応答を代替したテストで確認。
- v2でNetherNetエラーと `disconnectionscreen.notauthenticated` が解消し、Hiveに選択アカウントで入場できたことをユーザー確認済み。
