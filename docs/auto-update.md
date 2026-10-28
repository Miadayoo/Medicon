# DLL 自動アップデート（Loader 方式）

ユーザーは **MediconLoader.dll を一度だけ** 導入します。以後の本体（Core）の更新は、起動のたびに Loader が自動で取得・検証して読み込みます。配布物は Loader だけ、Core はサーバーに置きます。

実装状況：Phase 1〜5。サーバー配信・署名、Loader のダウンロード／検証／ロード／フォールバック、Core の C ABI、Admin タブからの公開、「アップデートされました」通知まで。実ゲームでの動作確認は未実施（下の「未検証」）。

## 2つの DLL

| DLL | 役割 | 配布先 |
|---|---|---|
| **MediconLoader.dll** | 更新の確認・ダウンロード・署名/ハッシュ検証・保存・Core のロードと起動。小さく安定を保つ | ユーザーへ配布 |
| **MediconCore.dll** | 既存の Medicon 本体（Hook・Module・ClickGUI・描画・Main Menu・Social など）。CMake の `Medicon` ターゲット（`Medicon.dll`）がそのまま Core。直接 inject もできる | サーバーに配置 |

Loader はパスで読み込むので、サーバー／Loader 上ではファイル名が `MediconCore.dll` になります。ビルドした `Medicon.dll` をそのままアップロードしてください。

## 流れ

1. ゲームに MediconLoader.dll が読み込まれる（既存の導入方法のまま）。
2. DllMain はワーカースレッドを1本作るだけ（ネットワークも LoadLibrary もしない。Loader Lock を避ける）。
3. ワーカーが `https://<host>/medicon/manifest.json` と `manifest.sig` を取得。
4. 埋め込んだ公開鍵で署名（RSA-PSS/SHA-256）を検証。失敗したら新しい Core は使わない。
5. manifest のバージョンが手元より新しければ Core をダウンロード。
6. サイズ一致と SHA-256 を確認。
7. `%LOCALAPPDATA%\Medicon\versions\<version>\` にアトミックに配置（検証が終わるまで正式版にしない。実行中の Core は上書きしない）。
8. いちばん新しい検証済み Core を `LoadLibraryExW`（絶対パス）で読み込み、`Medicon_GetAPIVersion` で API を確認し、`Medicon_Initialize` を呼ぶ。
9. 起動に失敗したら古い検証済み Core へフォールバック。オフラインなら手元の検証済み Core を起動。
10. どの Core も起動できなかったら、理由（MCBE バージョン違い、サーバーに届かない、読み込み失敗など）と `updater.log` の場所をダイアログで表示する。

`state.json` の `bad` に入るのは「読み込めたが API が違う／`Medicon_Initialize` が失敗した」Core だけです。Windows がファイルを読み込めなかった場合（パス、権限、ウイルス対策ソフト）はリリースのせいではないので、次回起動で再試行します。以前の Loader はこれも `bad` にしていたため、`bad_rule` が古い `state.json` の `bad` は一度クリアします。Core のパスは UTF-8 で受け渡すので、ユーザー名が日本語の `%LOCALAPPDATA%` でも読み込めます（以前の Loader は ANSI のパスを UTF-8 として読み、`LoadLibraryExW` に失敗していました）。

更新は **次回のゲーム起動時** に反映します。実行中の Core は入れ替えません（Hook や描画スレッドが参照を持つため）。Loader 自身の自己上書きもしません。

## サーバー（social-server）

静的ファイル配信で成立します。Node サーバー（social-server）は所有者の公開 API と CLI を足すだけです。

公開される URL（`127.0.0.1` で待ち受け、Cloudflare Tunnel 経由で `https://<host>` に出す）：

- `GET /medicon/manifest.json` … 最新版の manifest
- `GET /medicon/manifest.sig` … その署名（RSA-PSS/SHA-256、生バイト列に対して）
- `GET /medicon/releases/<version>/MediconCore.dll` … Core 本体
- `GET /medicon/releases/<version>/manifest.json` / `manifest.sig` … 版ごとの固定リンク

manifest の例：

```json
{
  "version": "1.0.5",
  "channel": "stable",
  "mcbe_version": "1.26.52",
  "loader_api": 1,
  "download_url": "https://<host>/medicon/releases/1.0.5/MediconCore.dll",
  "sha256": "…64 hex…",
  "size": 1234567,
  "changelog": ["+ Added auto update", "* Fixed Scaffold"],
  "published_at": 1790000000000
}
```

### 初回設定（秘密鍵はリポジトリにも配布物にも入れない）

```sh
cd social-server
npm run release:setup           # 鍵・保存先・.env・公開鍵ヘッダーを用意する
# 別の公開ドメインを使う場合:
# npm run release:setup -- --base-url https://<host>
```

- 秘密鍵 `keys/release-private.pem`：このサーバーで保持する（`.gitignore` 済み）。初回だけ作り、既存の鍵は再利用します。設定済みの鍵が消えている場合は、新しい鍵で置き換えずエラーにします。鍵だけを用意する従来の `npm run release:keygen` も利用できます。
- `.env`：既存設定を保持して `RELEASE_SIGNING_KEY` と `PUBLIC_BASE_URL` を補います。既定URLは Loader と同じ `https://medicon.auau1000.com` です。設定後はサーバーを再起動してください。サービス側に同名の環境変数がある場合は、そちらが優先されます。
- 公開鍵 `include/medicon/update/ReleasePublicKey.hpp`：コミットして **MediconLoader をビルドし直す**。`social-server/` だけを配置したサーバーでは `keys/ReleasePublicKey.hpp` に出力されるので、ビルド環境の同ヘッダーへ反映してください。Loader にはこの公開鍵だけが埋め込まれ、鍵が無いうちは Loader はどの Core も信頼しません。
- 鍵を作り直すと、既存の Loader は新しい署名を受け付けません（Loader の再配布が必要）。

### .env

```
RELEASE_SIGNING_KEY=keys/release-private.pem
PUBLIC_BASE_URL=https://<host>          # https://host のみ（パスなし）。manifest の download_url に使う
RELEASE_MCBE_VERSION=1.26.52            # 任意（既定 1.26.52）
RELEASES_DIR=./releases                 # 任意
```

両方（鍵と公開URL）が揃うと公開が有効になり、起動ログに `Core releases enabled` と出ます。

DLL は `RELEASES_DIR/<version>/MediconCore.dll` に保存し、同じフォルダに署名と manifest を置きます。既定の保存先は `social-server/releases/` です。サーバー移行時は DB・`.env` に加えて署名鍵と保存済み `releases/` も引き継いでください。

## 新しい版を公開する

### 方法A：クライアントの Admin タブ（owner）

1. Social 画面 → Admin タブ → Change Log を選ぶ。
2. **Attach DLL** を押すと Windows 標準のファイルを開くダイアログ（エクスプローラー）が出るので、ビルドした `Medicon.dll` を選ぶ。選んだパスは「DLL FILE」欄に表示され、その欄をもう一度押せば選び直せます。
3. Title・Version（`x.y.z`）・本文（Change Log）を入れる。
4. Post → 確認 → 公開。サーバーが署名付きで Core を保存し、同じ内容の Change Log も投稿します（どちらか片方だけにはなりません）。

Version は常に「今の最新より大きい」値にしてください。古い／同じ版は拒否されます。

送信前に `GET /releases/status`（owner のみ）で公開設定を確認します。未設定なら DLL を送らず `releases_disabled` を返します。投稿結果と失敗理由は Admin タブに次の投稿まで残り、失敗時には入力と添付ファイルを保持します。通信断・タイムアウトの場合は Change Log に反映されているか確認してから再試行してください。

### 方法B：サーバーの CLI

```sh
cd social-server
npm run admin -- release:publish --file ./Medicon.dll --version 1.0.5 \
  --title "Medicon 1.0.5" --body-file ./changelog.txt
```

### 方法C：静的ホスティング（social-server を使わない）

```sh
cd social-server
npm run release:sign -- --file ./Medicon.dll --version 1.0.5 \
  --key keys/release-private.pem --base-url https://<host> \
  --changelog-file ./changelog.txt --out ./out/static
```

`out/static/medicon/` をそのまま静的ホスト（Loader の許可ホスト上）へ置きます。

## Core の C ABI（`include/medicon/update/HostApi.h`）

```c
uint32_t Medicon_GetAPIVersion(void);              // == 1
int      Medicon_Initialize(const MediconHostAPI*);// 起動したら 1
void     Medicon_Shutdown(void);
```

`Medicon_Initialize` は Loader が渡す host 情報（Core のバージョン、直前のバージョン、データフォルダ、更新状態）を受け取り、既存のオーバーレイ起動（`MediconStartOverlay`）を呼びます。Loader が起動する場合は環境変数 `MEDICON_LOADER` を立てるので、Core の DllMain は自動起動せず、Loader からの `Medicon_Initialize` 一本に集約されます（直接 inject のときは今まで通り自動起動）。

更新状態が「更新された（1）」のときは、サインイン成功後に `アップデートされました（Updated to {version}）` の通知を Welcome 通知とあわせて1回だけ出します。再接続では出しません。

## セキュリティの要点

- 署名検証に失敗したら、ダウンロードした DLL を実行しない。SHA-256 だけを信頼の根拠にしない。
- HTTPS 固定。証明書検証は無効化しない。リダイレクトは拒否。
- `download_url` は `https://<許可ホスト>/medicon/releases/<version>/MediconCore.dll` に完全一致するものだけ受理（別ホスト・別パス・`..` を拒否）。
- サイズ上限（64 MiB）、サイズ不一致、タイムアウト、通信断を処理。
- バージョンはディレクトリ名になるため `x.y.z` 以外は拒否（トラバーサル防止）。
- 現在の MCBE バージョンと `loader_api` の互換を確認。

## 配布とビルド

- ユーザーへ：**MediconLoader.dll** のみ。
- サーバーへ：ビルドした **Medicon.dll**（アップロード時に MediconCore.dll として保存される）。

CustomModelのNeon（PNG 3枚）とChocolat v1.02（PNG 5枚）は、各FBX・設定とともにCore DLLのresourceへ同梱する。
Loaderによる別のモデルダウンロードやDLL隣への手動コピーは不要。
Coreのworkerが`%LOCALAPPDATA%/Medicon/cache/models/<モデル名>-<pack SHA-256>/`へ検証・展開して読み込む。
CustomModelのModel項目でNeon／Chocolatを選べる。有効な外部`models/neon-t-pose`または`models/chocolat-v1.02`がある場合はそちらを優先する。
同梱対応前の配信済みDLLは変わらないため、この修正を含むCoreを新しいバージョンで公開する必要がある。

```powershell
# Windows
pwsh tools/build.ps1            # Medicon.dll と MediconLoader.dll を out/build/*/bin に出す
```

`MEDICON_UPDATE_HOST`（既定 `medicon.auau1000.com`）で Loader の接続先を、`MEDICON_SOCIAL_SERVER_URL` で Core の Social 接続先を指定します。

## テスト

- `social-server`：`npm test`（`tests/releases.test.ts` … 署名・SHA・バージョン規則・トラバーサル拒否・owner 限定・一括投稿）。
- 移植可能な更新ルール：`ctest -R update`（バージョン比較、manifest 検証、状態管理、prune）。
- Loader のオーケストレーション：`ctest -R loader`（初回・最新据え置き・オフライン・署名不正・ハッシュ不一致・DL中断・起動失敗フォールバック・改竄ローカル・MCBE不一致）。
- クライアント UI：`preview_social_admin_dll`（DLL 添付投稿）、`preview_social_update_notice`（更新通知）。

## 未検証

実ゲーム（Minecraft.Windows.exe 1.26.52）での注入・ダウンロード・ロード・フォールバック・通知は未確認です。WinHTTP/BCrypt/ファイル処理の Windows 実装はこの作業環境ではビルドできないため、移植可能なロジックと合成テストでのみ検証しています。初回はテスト用の鍵と版で、方法C（静的ホスティング）か検証用サーバーで一通り確かめてください。
