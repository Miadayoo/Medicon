# Medicon Social Server

Medicon のクライアント内SNS「Social」のサーバー（実装指示書の Part A）です。REST と WebSocket を Fastify + ws で提供し、自宅PCの `127.0.0.1` だけで待ち受けて、外部には Cloudflare Tunnel 経由の `wss://` として公開します。通信の形式は実装指示書「2. 共通仕様」に従います。

**実装状況：フェーズ1〜6。** DB、ライセンスキー認証、管理CLIと管理ポート、WebSocket、お知らせ（news / changelog）、Global チャット、フレンド、DM（既読・未読・オンライン状態）。`msg:delete` とその反映はフェーズ7で追加します。

CustomModel 共有にも対応しています。認証済み接続の `model.sync` / `model.peers` で、近くのユーザーの Neon / Chocolat の選択を同期します。クライアントと本サーバーの両方を更新して再起動してください。DB の変更・移行はありません。共有状態は接続中のメモリだけに保持し、切断・OFF・15秒無更新で解除します。プロキシで ActorUniqueID が食い違う場合に備え、ID と名前の組で一致しないときは一意な名前で照合します（この変更もサーバーの再起動が必要です）。詳細は [クライアントの共有仕様](../docs/social.md) を参照してください。

| 機能 | REST | WebSocket |
|---|---|---|
| Global | `GET /global/messages?limit=50&before=<id>` | `global.send` → `ack`（message 付き）、全員に `global.new` |
| DM | `GET /dm/{userId}/messages`（フレンドでなければ 403 `not_friends`） | `dm.send` → `ack`、相手に `dm.new`。`dm.read`（ack なし）→ 相手に `dm.read` |
| フレンド | `GET /friends`、`GET /users/search?q=<前方一致>`（最大10件、自分を除く） | `friend.request` / `friend.respond` / `friend.remove` → `ack`、相手に `friend.request` / `friend.update`。接続と切断で、オンラインのフレンドに `presence` |

制限（実装指示書 2.5）：本文は前後の空白を除いて1〜300コードポイント（`empty` / `too_long`）。Global は1ユーザー0.3秒に1回（仕様の2秒から短縮）、DM は5秒に5回、フレンド申請は1時間に10回（`rate_limited`）。相手から自分への申請があるときに申請すると、その場でフレンドになります。

## Core DLL の自動アップデート

MediconLoader に配る Core DLL の配信も、このサーバーで行えます。初回に `npm run release:setup` を実行してサーバーを再起動してください。署名鍵・保存先と `.env` の `RELEASE_SIGNING_KEY` / `PUBLIC_BASE_URL` を用意し、owner の Admin タブから DLL を公開できるようにします。既定の公開URLは `https://medicon.auau1000.com` です。別のドメインでは `npm run release:setup -- --base-url https://<host>` を使います。既存の鍵・管理用シークレット・DB設定は引き継ぎます。

初回は生成された公開鍵ヘッダーで **MediconLoader をビルドし直して配布**してください。サーバーだけを配置した場合は `keys/ReleasePublicKey.hpp` に出力されるので、ビルド環境の `include/medicon/update/ReleasePublicKey.hpp` に反映します。DLL は `releases/<version>/MediconCore.dll` に永続保存され、再起動後も配信されます。詳細は [../docs/auto-update.md](../docs/auto-update.md) を参照してください。

## 必要なもの

- Node.js 24 LTS 以降
- 公開する場合：Cloudflare アカウントと、Cloudflare で管理しているドメイン

## セットアップ

### Node.js

このPCでは `.tools\node-v24.22.1-win-x64` にポータブル版を置いています（Git管理外）。PowerShell では使う前に PATH に加えます。

```powershell
$env:Path = "D:\code\Medicon\social-server\.tools\node-v24.22.1-win-x64;$env:Path"
```

別のPCでは Node.js 24 LTS をインストールするか、[nodejs.org](https://nodejs.org/) の `node-v24.x.x-win-x64.zip` を同じ場所に展開してください。TypeScript は Node.js の型除去機能でそのまま実行するので、ビルドは不要です。

PowerShell で「スクリプトの実行が無効」と言われて `npm.ps1` が動かない場合は、`npm` の代わりに `npm.cmd` を使ってください（例：`npm.cmd run admin -- license:list`）。

### 依存パッケージと .env

```powershell
npm install
Copy-Item .env.example .env
```

`.env` の `ADMIN_SECRET` に16文字以上のランダムな文字列を書きます。次のコマンドで作れます。

```powershell
node -e "console.log(require('node:crypto').randomBytes(32).toString('base64url'))"
```

| 変数 | 既定値 | 内容 |
|---|---|---|
| `PORT` | `8787` | メインサーバー（REST + WebSocket）。`127.0.0.1` だけで待ち受ける |
| `ADMIN_PORT` | `8788` | 管理用HTTP。`127.0.0.1` だけで待ち受け、Tunnel には載せない |
| `DB_PATH` | `./data/social.db` | SQLite（WAL）。相対パスはこのフォルダが基準 |
| `ADMIN_SECRET` | なし（必須） | 管理APIの `X-Admin-Secret`。16文字以上 |
| `BACKUP_DIR` | `./backups` | 毎日のバックアップ先 |

### 起動

```powershell
npm start
```

`listening on http://127.0.0.1:8787 (admin http://127.0.0.1:8788)` と出れば起動しています。Ctrl+C を押すと、全接続を close 1001 で切ってから DB を閉じて終了します。開発中は `npm run dev` でファイル保存時に再起動します。

### Linux（start.sh）

Linux 版の Node.js 24 以上と npm を用意して、このフォルダをサーバーへ転送します。Windows の `.tools` と `node_modules` は転送不要です。

```sh
cd /opt/social-server  # 実際の配置先に変更
sh ./start.sh
```

`start.sh` は呼び出した場所に関係なく、このフォルダを基準に起動します。依存パッケージが未導入、または SQLite のネイティブモジュールが実行環境と合わない場合は `npm ci --omit=dev` で入れ直します。`.env` がなければランダムな `ADMIN_SECRET` を設定して作成し、既存の `.env` と DB は引き継ぎます。更新後に依存パッケージを確実に揃える場合は、先に `npm ci --omit=dev` を実行してください。

既存のライセンスキーを別のサーバーでも使うには、移行元を停止してから `data/` と `.env` も転送してください。DLL配信も移行する場合は `keys/` と `releases/`（設定で保存先を変えている場合はそのフォルダ）も転送します。署名鍵を作り直すと既存の Loader が更新を検証できなくなります。`data/` がなければ空の DB で起動します。

SSH を切断しても動かすだけなら、次のように起動できます。

```sh
mkdir -p logs
nohup sh ./start.sh >logs/server.log 2>&1 < /dev/null &
```

再起動後も自動起動させる場合は、後述の「サーバーの常駐化 → Linux（systemd）」を使ってください。外部公開は後述の Cloudflare Tunnel で `http://127.0.0.1:8787` に転送します。

## 管理CLI

サーバーを起動した状態で、別のターミナルから実行します。CLI は DB を直接書かずに管理ポートを呼ぶので、切断やお知らせの配信がその場で反映されます。

```powershell
npm run admin -- license:create --name faokun
```

| コマンド | 動作 |
|---|---|
| `license:create --name <name>` | ユーザーを作成（既にいればキーだけ追加）してキーを表示する。キーは DB にハッシュだけを保存するので、表示はこの1回だけ |
| `license:revoke --name <name>` | そのユーザーの全キーを無効化し、セッションを削除する。接続中なら `kicked` を送って close 4003 |
| `license:list` | username、BAN、キーの状態、作成日時、最終使用日時 |
| `user:ban --name <name>` | BAN してセッションを削除する。接続中なら `kicked`（banned）を送って close 4003 |
| `user:unban --name <name>` | BAN を解除する。キーはそのまま使える |
| `news:post --kind <news\|changelog> --title <t> --body-file <path> [--version <v>]` | 保存して全接続に `news.new` を配信する。本文は UTF-8 のテキストファイル |
| `user:rank --name <name> --rank <owner\|user>` | ランクを変える。接続中なら close 4001（`rank_changed`）で切り、クライアントは再接続して新しいランクで入り直す |

### ランク

ユーザーには `user`（既定）と `owner` のランクがあります（`users.rank` 列。古い DB は起動時に列を追加します）。ランクはログインの応答、`hello` の `user`、メッセージの `author` に `rank` として入ります。

owner は WebSocket の `news.post`（`{t, id, kind, title, body, version?}`）でお知らせと Change Log を投稿できます。保存後に `ack` を返し、全接続へ `news.new` を配信します。owner 以外は `forbidden`、タイトルや本文が空なら `bad_request` です。本文の改行は LF にそろえます。

username は3〜16文字の英数字と `_` です。大文字小文字を区別せずに一意で、表示は最初に登録した表記になります。

## test-client（DLLなしでの確認）

```powershell
npm run test-client
```

ライセンスキーを聞かれるので貼り付けます。`--key` でも渡せますが、npm が実行するコマンド行を表示するので、画面にキーが残ります。別のサーバーにつなぐときは `npm run test-client -- --server https://social.example.com` です。

接続すると、受信したフレームを `<` 付きでそのまま表示します。ping は30秒ごとに自動で送ります。

| コマンド | 動作 |
|---|---|
| `/news [news\|changelog]` | 最新20件を取得して表示 |
| `/g <本文>` | Global に投稿 |
| `/global` | Global の最新20件 |
| `/friends` | フレンドと申請の一覧 |
| `/search <前方一致>` | ユーザーを検索 |
| `/add <name>` | フレンド申請 |
| `/accept <userId>` / `/decline <userId>` | 届いた申請を承認／拒否 |
| `/remove <userId>` | フレンド削除、または送った申請の取消 |
| `/dm <userId> <本文>` | DM を送る |
| `/dmh <userId>` | DM の最新20件 |
| `/read <userId> <msgId>` | DM を msgId まで既読にする |
| `/ping` | ping を送る |
| `/raw <text>` | 任意のテキストフレームをそのまま送る（不正な形式の確認用） |
| `/help` | コマンド一覧 |
| `/quit` | 終了 |

### フェーズ1の完了条件を確かめる手順

1. ターミナル1：`npm start`
2. ターミナル2：`npm run admin -- license:create --name tester` を実行し、表示されたキーを控える
3. ターミナル3：`npm run test-client` でキーを入力する。`< {"t":"hello",...}` が出る
4. ターミナル2：本文を書いた `notice.txt` を用意して `npm run admin -- news:post --kind news --title "テスト" --body-file .\notice.txt` を実行する。ターミナル3に `< {"t":"news.new",...}` が出る
5. ターミナル2：`npm run admin -- license:revoke --name tester` を実行する。ターミナル3に `< {"t":"kicked","reason":"revoked"}` と `切断されました: code=4003` が出る

## テスト

```powershell
npm test
npm run typecheck
```

テストはメモリ上の DB と `127.0.0.1` の空きポートでサーバーを起動し、次を確認します。

- 認証：正常、無効なキー、revoke、BAN と解除、`CF-Connecting-IP` ごとのレート制限、トークンの期限、ハッシュだけの保存
- news：種類ごとの一覧、`limit` / `before`、`limit` の上限、不正な値
- WebSocket：Upgrade の401拒否、hello、ping/pong、`bad_request` の ack、close 4009 / 4003 / 4001 / 1001、`news.new` の配信、無通信での切断
- 管理API：`X-Admin-Secret`、ユーザー作成とキーの追加、username の検証、一覧
- バックアップ：読めるコピーの作成と14個の保持

- Global：投稿と配信（送信者にも）、ack の message、空と301文字（コードポイントで数える）、0.3秒の連投制限、新しい順の履歴と `before`
- フレンド：申請と通知、承認と presence（双方）、相互申請で即フレンド、拒否、取消、削除、エラー（self / user_not_found / request_exists / already_friends）、前方一致検索と relation
- DM：フレンド以外への DM と履歴の拒否、送信と配信、未読数、既読（後戻りしない）と theirReadUpTo、5秒に5回の制限

DLL と組み合わせた確認は、リポジトリの `tools/social-e2e.mjs`（フェーズ3）と `tools/social-chat-e2e.mjs`（フェーズ4〜6）で行います（`docs/social.md`）。

## Cloudflare Tunnel で公開する

ルーターのポート開放は不要で、自宅のIPも公開されません。サーバーは `127.0.0.1` でしか待ち受けないので、外部からは Tunnel 経由でしか届きません。Tunnel はメインサーバー（8787）だけを転送し、管理ポート（8788）は載せません。

以下の `social.example.com` は実際のドメインに置き換えてください。

1. Cloudflare にログインする（ブラウザが開くのでドメインを選ぶ）

   ```
   cloudflared tunnel login
   ```

2. トンネルを作る（認証情報 `<Tunnel-ID>.json` が `.cloudflared` フォルダにできる）

   ```
   cloudflared tunnel create medicon-social
   ```

3. DNS を割り当てる

   ```
   cloudflared tunnel route dns medicon-social social.example.com
   ```

4. `config.yml` を書く。最後の行には必ず `http_status:404` を置く

   ```yaml
   tunnel: <Tunnel-ID>
   credentials-file: <認証情報のフォルダ>/<Tunnel-ID>.json
   ingress:
     - hostname: social.example.com
       service: http://127.0.0.1:8787
     - service: http_status:404
   ```

5. サービスとして常駐させる

   ```
   cloudflared service install
   ```

WebSocket はそのまま Tunnel を通ります（Cloudflare ダッシュボードの Network にある WebSockets が有効なこと。既定で有効）。Cloudflare は約100秒無通信の WebSocket を切るので、クライアントは30秒ごとに ping を送り、サーバーは75秒受信がなければ切断します。

### Windows でのサービス化

Windows のサービスはユーザープロファイルではなく `C:\Windows\System32\config\systemprofile\.cloudflared\` を見ます。手順1〜3のあと、管理者のコマンドプロンプトで次のようにします（[公式手順](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/configure-tunnels/local-management/as-a-service/windows/)）。

1. `cloudflared.exe service install`
2. `mkdir C:\Windows\System32\config\systemprofile\.cloudflared`
3. `%USERPROFILE%\.cloudflared\` の `cert.pem` と `<Tunnel-ID>.json` を、2のフォルダにコピーする
4. 2のフォルダに `config.yml` を置き、`credentials-file` をコピー先の `<Tunnel-ID>.json` にする。`cloudflared tunnel --config C:\Windows\System32\config\systemprofile\.cloudflared\config.yml ingress validate` で確認できる
5. レジストリエディタで `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\Cloudflared` の `ImagePath` を次の形にする（`cloudflared.exe` のパスは実際の場所に合わせる）

   ```
   C:\Cloudflared\bin\cloudflared.exe --config=C:\Windows\System32\config\systemprofile\.cloudflared\config.yml tunnel run
   ```

6. `sc start cloudflared`。設定を変えたら `sc stop cloudflared` → `sc start cloudflared`

### Linux でのサービス化

`~/.cloudflared/config.yml` に `tunnel` と `credentials-file` を書いたうえで、`sudo` では `$HOME` が `/root` になるので設定ファイルを明示します（[公式手順](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/configure-tunnels/local-management/as-a-service/linux/)）。

```sh
sudo cloudflared --config /home/<USER>/.cloudflared/config.yml service install
sudo systemctl start cloudflared
```

## サーバーの常駐化

### Windows（NSSM）

[NSSM](https://nssm.cc/) を用意し、管理者の PowerShell で実行します。パスは実際の場所に合わせてください。

```powershell
$dir  = "D:\code\Medicon\social-server"
$node = "$dir\.tools\node-v24.22.1-win-x64\node.exe"
New-Item -ItemType Directory -Force "$dir\logs" | Out-Null
nssm install medicon-social $node "$dir\src\index.ts"
nssm set medicon-social AppDirectory $dir
nssm set medicon-social AppStdout "$dir\logs\server.log"
nssm set medicon-social AppStderr "$dir\logs\server.log"
nssm set medicon-social AppRotateFiles 1
nssm set medicon-social AppRotateOnline 1
nssm set medicon-social AppRotateBytes 10485760
nssm set medicon-social AppStopMethodConsole 5000
nssm start medicon-social
```

`AppStopMethodConsole` により、停止時は Ctrl+C を送って最大5秒待つので、接続を close 1001 で切ってから終了します。状態の確認は `nssm status medicon-social`、再起動は `nssm restart medicon-social` です。

### Linux（systemd）

`/etc/systemd/system/medicon-social.service`：

```ini
[Unit]
Description=Medicon Social server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=medicon
WorkingDirectory=/opt/social-server
ExecStart=/usr/bin/node /opt/social-server/src/index.ts
Restart=on-failure
RestartSec=5
TimeoutStopSec=15

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now medicon-social
journalctl -u medicon-social -f
```

停止時は SIGTERM で同じ終了処理が走ります。

## バックアップ

- 起動時（前回のバックアップから1日以上経っていれば）と、その後1日1回、better-sqlite3 の `db.backup()` で `BACKUP_DIR` に `social-YYYYMMDD-HHMMSS.db` を作ります。
- 新しいものから14個（約14日分）を残し、古いものは消します。
- 復元するときは、サーバーを止めてから `data\social.db`・`social.db-wal`・`social.db-shm` を退避し、バックアップを `data\social.db` としてコピーして起動します。

## ログ

起動と終了、WebSocket の接続数、エラーだけを標準出力と標準エラーに出します。メッセージ本文、ライセンスキー、トークンは出しません。

## 構成

```
src/
  index.ts            起動・終了処理
  app.ts              公開サーバー（Fastify と /ws の Upgrade）
  context.ts          DB・接続ハブ・時計・レート制限の共有
  config.ts           .env の読み込みと検証
  errors.ts           エラーコード（共通仕様 2.5）
  log.ts              ログ
  ratelimit.ts        レート制限（スライディングウィンドウ）
  db/schema.sql       起動時に適用するスキーマ
  db/db.ts            SQLite（WAL、外部キー有効）
  db/backup.ts        毎日のバックアップ
  auth/license.ts     キーの生成・正規化・照合
  auth/session.ts     トークンの発行・検証
  http/common.ts      認証、エラーの形式、ページング
  http/routes/        auth, news
  ws/protocol.ts      フレームの型と close コード
  ws/hub.ts           接続管理（userId → socket）
  services/           users, news
  admin/server.ts     管理用HTTP（ADMIN_PORT）
admin/cli.ts          管理CLI
tools/test-client.ts  確認用のCLIクライアント
tests/                vitest
```
