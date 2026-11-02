# Alt Manager: 1.26.52.03 認証 SDK 調査

現行の1.26.60.5対応は [更新記録](bedrock-1.26.60.md) と
[新しい認証証拠](bedrock-1.26.60.05-auth-evidence.json) を参照。以下は旧版の調査履歴。

調査日: 2026-10-31 (JST)。この文書は実装用の静的解析メモで、切り替え機能の動作保証ではない。

## 対象と検証範囲

- 対象: LeviLauncher の `versions/1.26.52.03/Minecraft.Windows.exe`。
- PE image base: `0x140000000`。以下の VA はこの基準。実行時には ASLR 後の base に RVA を足す。
- ファイル長: `311218032` bytes。
- SHA-256: `47de440dc290bb5bd2a4ccd79e39c00219a155ea2c23bb961e27e323bd2fd942`。
- `.levildr` section と `LeviLauncher.dll` import を含むファイル。未加工の配布 exe と同一とは扱わない。
- 初回の IDA MCP は応答せず、同じパスの PE を `.pdata` と Capstone で直接解析した。その後ユーザーの MCP 再起動により復旧。**IDA MCP で input SHA-256、9 関数の境界・sig バイト列・xref、主要関数の Hex-Rays 出力を照合済み**。IDA の input hash も上記と一致した。
- 各 sig の `.text` 内一致数は 1。sig、VA、RVA、関数終端は [JSON](alt-manager-1.26.52.03-signatures.json) に保存した。
- IDA の元の仮名・型を残した [逆コンパイル抜粋](alt-manager-1.26.52.03-ida-evidence.json) も保存した。decompiler の自動推定が未使用引数を省略する場合があるため、ABI 表は call-site の逆アセンブルを優先している。
- ゲームプロセスの token の読み出し、アカウント変更、パケット送信、フック導入は行っていない。

## 得られたもの

| 項目 | 結果 |
| --- | --- |
| 認証パケット生成地点 | `ConnectionRequest::create` 相当: `0x140479280`, RVA `0x479280` |
| ABI | Windows x64。RCX は呼び出し側の戻り値バッファ、R8 が `ConnectionAuthInfo`。合計 26 の物理引数スロット |
| Xbox token の取得 | `Social::XboxLiveIdentity::getAuthToken` → XAL 互換 wrapper → GDK `XUserGetTokenAndSignature*` 相当 |
| ゲームサーバー用 token の取得 | `PlayerAuthentication::getGlobalToken` → `AuthServiceApi::startMultiplayerSession` → `AuthServiceMultiplayerSession` → `RawGameServerToken` |
| ユーザー情報 | `Social::UserImpl +0x218` の identity collection から Xbox identity (key `1`) を取得。`XboxLiveIdentity +0x30` が十進 XUID、`+0x1D8` が gamertag |

## ConnectionRequest 生成の sig と ABI

関数範囲は `[0x140479280, 0x140479E50)`。既存の Solstice 候補もこの位置だけに一致した。

```text
55 41 57 41 56 56 57 53 48 81 EC B8 01 00 00
48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 4C 89 CB
```

呼び出し元は `0x141330030`。埋め込み関数名文字列により
`LegacyClientNetworkHandler::handle(const NetworkIdentifier &, const NetworkSettingsPacket &)` と特定できる。
`0x1413313E5` で上記関数を直接呼ぶ。

以下の `S` は生成関数への **entry 時点の RSP**。項目名は既存 SDK と JSON フィールドへの書き込みを突き合わせた意味名。
ポインタで渡される C++ aggregate の完全な型定義は未復元である。

| 物理引数 | 場所 | 内容・機械型 |
| --- | --- | --- |
| 1 | RCX | 戻り値 `ConnectionRequest` の格納先 (`void*`) |
| 2 | RDX | AppPlatform の NonOwnerPointer 一時 aggregate へのポインタ |
| 3 | R8 | `ConnectionAuthInfo*` |
| 4 | R9 | SelfSignedId: MSVC `std::string` へのポインタ |
| 5 | `[S+0x28]` | ServerAddress: string ポインタ |
| 6 | `[S+0x30]` | ClientRandomId: 64-bit |
| 7 | `[S+0x38]` | ConnectionSkinInfo aggregate ポインタ |
| 8 | `[S+0x40]` | DeviceId: string ポインタ |
| 9 | `[S+0x48]` | CurrentInputMode: 32-bit |
| 10 | `[S+0x50]` | GuiScale: 32-bit |
| 11 | `[S+0x58]` | LanguageCode: string ポインタ |
| 12 | `[S+0x60]` | ClientIsEditorCapable: bool |
| 13 | `[S+0x68]` | ClientEditorConnectionIntent: 32-bit |
| 14 | `[S+0x70]` | IsEduMode: bool |
| 15 | `[S+0x78]` | optional WebToken aggregate ポインタ |
| 16 | `[S+0x80]` | EduSessionToken: string aggregate ポインタ |
| 17 | `[S+0x88]` | EduJoinerToHostNonce: string aggregate ポインタ |
| 18 | `[S+0x90]` | ADRole: byte |
| 19 | `[S+0x98]` | PlatformUserId: string ポインタ |
| 20 | `[S+0xA0]` | ThirdPartyName: string ポインタ |
| 21 | `[S+0xA8]` | PlatformOnlineId: string ポインタ |
| 22 | `[S+0xB0]` | PlatformOfflineId: string ポインタ |
| 23 | `[S+0xB8]` | optional PlayerPartyInfo aggregate ポインタ |
| 24 | `[S+0xC0]` | CompatibleWithClientSideChunkGen: bool |
| 25 | `[S+0xC8]` | SyncedClientOptionsComponent ポインタ |
| 26 | `[S+0xD0]` | optional Nonce aggregate ポインタ |

`0x1413313CD` は RCX に caller の stack buffer、`0x1413313DB` は R8 に auth aggregate のアドレスを入れる。
生成関数は `0x140479C65` でこの buffer に構築し、`0x140479DE1` でそのアドレスを RAX に戻す。
既存 hook の第1引数 `_this` という名前を、そのまま instance pointer と解釈してはいけない。

RDX の aggregate は関数内で参照カウントを解放する。引数 15〜17 も後処理される。
単に全項目を C++ の参照型にした宣言は、元の by-value aggregate の所有権処理を再現しない。
フック導入前には完全な C++ ABI と一時オブジェクトの所有権を追加検証する。

生の物理 ABI を表す function pointer は次の形。各ポインタ先を自動構築・破棄する C++ SDK の代用ではない。

```cpp
using ConnectionRequestCreateRaw = void* (__fastcall *)(
    void* result, void* platformTemporary, const void* authInfo,
    const void* selfSignedId, const void* serverAddress,
    std::uint64_t clientRandomId, const void* skinInfo, const void* deviceId,
    std::uint32_t inputMode, std::uint32_t guiScale, const void* languageCode,
    std::uint8_t editorCapable, std::uint32_t editorIntent, std::uint8_t eduMode,
    void* eduTokenTemporary, void* eduSessionTemporary, void* eduNonceTemporary,
    std::uint8_t role, const void* platformId, const void* thirdPartyName,
    const void* platformOnlineId, const void* platformOfflineId,
    const void* partyInfo, std::uint8_t chunkGenCompatible,
    const void* syncedOptions, const void* nonce);
```

### ConnectionAuthInfo の観測配置

```cpp
// 部分復元。signingKey の具体型・所有権は未確定。
struct ConnectionAuthInfoObserved {
    void* signingKey;                  // +0x00
    std::int32_t authenticationType;   // +0x08
    std::uint32_t padding;             // +0x0C
    // MSVC release std::string         // +0x10, 0x20 bytes
};                                    // caller 上の領域は 0x30 bytes
```

`0x14046ADD0` はこの aggregate の JSON 化処理。
`0x14046ADF2` で `+0x10` を取り `Token` として格納し、
`0x14046AE26` で `+0x08` の dword を読み `AuthenticationType` として格納する。
`0x140479B76` は `+0x00` のポインタを取り、payload の WebToken 構築側へ渡す。

呼び出し元では `0x141330D25` 以降で
`LegacyClientNetworkHandler +0x350` の認証種別と `+0x358` の token string をこの aggregate にコピーする。

### 古い ConnectionRequest 配置との違い

今回の生成結果には vptr がある。`0x14047A770` の vtable 書き込みは `0x14E75E2E8` を使用する。
base 構築処理 `0x1404638F0` は、optional WebToken を `+0x08`、token string を `+0x90`、
認証種別 dword を `+0xB0` に構築する。
既存参照 SDK の「unique_ptr 3 個、clientSubId」という配置を、この版の出力に適用できない。

## token 取得経路と形式

### Xbox 認証

1. `Social::XboxLiveIdentity::getAuthToken`: `0x1413C92F0`。
2. `0x1413C93B8` → helper `0x141392740`。対象 URL の string を入力に取る。
3. wrapper callback `0x1413BF4C0`。RTTI の名前に
   `Social::XalApiWrapper::userGetTokenAndSignatureSilentlyAsync(XUser*, const XalUserGetTokenAndSignatureArgs*, XAsyncBlock*)` が残る。
4. `0x1413BF50E` → runtime adapter `0x14E0225CC`。ここで 9 引数の GDK `XUserGetTokenAndSignatureAsync` 形式へ展開される。
5. 完了 helper `0x1413BDD80` → callback `0x1413BF5D0` (ResultSize) と `0x1413BF560` (Result)。
   runtime adapter はそれぞれ `0x14E0226B0`, `0x14E022730`。

これは「独自 OAuth だけ」の実装ではなく、**ゲームの XAL wrapper が GDK XUser API の引数へ変換する経路**である。
adapter は取得した runtime interface の vtable を呼ぶ。通常 import table に `XUserGetTokenAndSignatureAsync` がないことは、未使用の証拠にならない。

Result の data は x64 で `tokenSize +0x00`, `signatureSize +0x08`, `token +0x10`, `signature +0x18`。
`0x1413BE079` 以降が data `+0x10` の token 文字列を取り出す。
この配置は [Microsoft の構造体定義](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xuser/structs/xusergettokenandsignaturedata?view=gdk-2510) とも一致する。
取得対象 URL と RP の対応は [XUserGetTokenAndSignatureAsync の仕様](https://learn.microsoft.com/en-us/xbox/gdk/docs/reference/system/xuser/functions/xusergettokenandsignatureasync?view=gdk-2604) に従う。

### ゲームサーバー用 token

Xbox 向け X-token と `ConnectionAuthInfo::Token` を同一視しない。

```text
ClientInstance::setupClientGame                  0x145DAD6B0
  -> PlayerAuthentication::getGlobalToken        0x146175C30
     -> AuthServiceApi::startMultiplayerSession  0x144282E30
        -> AuthServiceMultiplayerSession
     -> RawGameServerToken
  -> ClientGameSetupResult continuation
```

- `getGlobalToken` の直接呼び出しは `0x145DADED1`。
- `0x146175D36` は AuthService interface の vtable `+0x68` を呼ぶ。
  vtable `0x14E8C2FB0 +0x68` の値は `0x144282E30` で、同関数内に `AuthServiceApi::startMultiplayerSession` の名前文字列がある。
- 戻り値から `AuthServiceMultiplayerSession → RawGameServerToken` への task 型名も `getGlobalToken` 内に残っている。
- その変換 continuation は `0x1461931C0`。サービス結果の `+0x08` の string を `RawGameServerToken` に取り出す。
- `ClientInstance::setupClientGame` の continuation は `0x145DDE400`。global token が空でなければ `0x145DDE54C` で `setupGame(client, 0, token)` 相当を呼ぶ。空で、旧方式が許される場合は `0x145DDE58C` で `setupGame(client, 2, legacyToken)` 相当へ進む。**この 0 / 2 は setup helper の実引数として確認した値**であり、未追跡の中間層を省略して全 enum の名称を断定するものではない。
- 同じ exe に `api://auth-minecraft-services/multiplayer` (`0x14EC00B19`) と `signedToken` (`0x14EC2C917`) が存在する。
  audience 文字列の存在だけで全呼び出しの使用先までは確定しない。
- `Social::MinecraftServiceIdentity::getAuthToken` は別関数 `0x141375D00` に存在する。

Mojang の [NetherNet 公式資料](https://github.com/Mojang/bedrock-protocol-docs/blob/main/additional_docs/NetherNetOnboardingGuide.md#5-identity-assertions) も、
Minecraft auth service 発行の GameServerToken が JWT であり、XUID 等と client 公開鍵 `cpk` を含むことを説明している。
これは形式の補助根拠で、今回の Legacy/RakNet 呼び出しの実行観測の代用ではない。

現在の Alt Manager がアカウント表示用に取得する `RelyingParty=http://xboxlive.com` の XSTS だけでは、
この GameServerToken を得たことにならない。実装に進むには、自前アプリの正規ログインから Minecraft 側の token を取得し、
ゲームの署名鍵との対応を保てることを検証する必要がある。
実際の token の `iss` / `aud` / `exp`、認証種別の選択値、サービスの発行条件は未実測。

## Social::User 相当の配置

具体クラスは `Social::UserImpl`。`create` は `0x140E47780`、vtable は `0x14E791410`。
`create` 内で 0x4F8 bytes の shared control block 込み領域を確保し、`block+0x10` にこの vtable を置く。

| 対象 | offset / slot | 観測内容 |
| --- | --- | --- |
| UserImpl | `+0x218` | identity collection へのポインタ |
| UserImpl virtual | vtable byte offset `+0xE8` / index 29 | identity type を指定して `shared_ptr<Identity>` を返す (`0x140E57870`) |
| Identity collection node | `+0x10` | identity type key (観測は 64-bit 比較) |
| Identity collection node | `+0x18`, `+0x20` | identity object pointer / shared control block pointer |
| XboxLiveIdentity | key `1`, vtable `0x14E7B4A00` | Xbox アカウントの identity |
| XboxLiveIdentity | `+0x30` | 十進 XUID の MSVC string |
| XboxLiveIdentity | `+0x1C8` | XboxLiveUser へのポインタ |
| XboxLiveIdentity | `+0x1D8` | gamertag の MSVC string |
| XboxLiveUser | `+0x78` | XUser handle。token helper に渡す値 |

identity getter の機械 ABI は `RCX=UserImpl*`, `RDX=shared_ptr の戻り値領域`, `R8D=identity type`。
`0x14428A430` が collection から identity を検索する。
map の内部を固定長配列として扱わず、取得後の vtable と所有権を確認する。

XUID と gamertag の根拠は `0x1413D5FA0` の sign-in 後 continuation:

- `0x1413D6117` → XUser ID helper `0x1413911C0`。
- `0x1413D6130` は変換基数 `10`。その結果を `0x1413D6241` 以降で `XboxLiveIdentity +0x30` に格納する。
- `0x1413D62A2` → gamertag helper `0x1413913A0`。
- `0x1413D633E` 以降で `XboxLiveIdentity +0x1D8` にその結果を格納する。

現ユーザーへの入口候補も、ログイン生成 caller と PlayerAuthentication の両方にある。
ClientInstance 側 vtable `+0x598` の呼び出し結果を shared_ptr のように扱い、先頭 pointer を User として読む
(`0x141331015`〜`0x141331022`, `0x146175CB6`〜`0x146175CE2`)。
この slot の名前と実行時の concrete ClientInstance の一致は追加確認が必要。

ここで使われる string は MSVC release 配置で 0x20 bytes。
`+0x10` が長さ、`+0x18` が capacity、capacity が `0x10` 未満なら先頭16 bytes が inline buffer。
DLL の別 CRT にある `std::string` と相互に move / delete してよいという意味ではない。

## 未完了の検証

1. 戻り値 aggregate、optional、NonOwnerPointer の完全な ABI / 破棄規則を確認する。
2. setup helper から `LegacyClientNetworkHandler +0x350/+0x358` までの中間層を追い、認証種別の enum 名・他の値を確定する。
3. 自前 Entra アプリでの Minecraft token 発行可否、署名鍵との結び付け、expiry / refresh を実ログインで検証する。
4. UserImpl の identity 変更通知・XSAPI context 等の更新と、再接続後のサーバー側 XUID / gamertag を確認する。

この調査だけで `Use` ボタンを有効化しない。既存のゲームコード・署名カタログ・認証実装は変更していない。

## 初回に実装した範囲（2026-10-31、接続実装前）

資料の留保（全 sig `runtime_validated=false`、手元にゲーム本体が無く静的解析のみ、GameServerToken の発行可否が未実測、`Use` を有効化しない）に従い、**安全・検証可能・レビュー可能な土台だけ**を追加した。アカウントの書き換え・切り替え・token 差し替え・ゲーム関数の呼び出しは一切入れていない。

| 追加物 | 内容 |
| --- | --- |
| 署名カタログ | `data/signatures/medicon-auth.json` と `src/memory/medicon_auth_catalog.inc` に 9 関数を登録。`medicon_auth_signatures()` で取得。全エントリ `upstream_uncertain=true`（runtime 未検証）。provenance は `project_research` |
| 読み取り専用 SDK | `medicon::sdk::BedrockAuth`（`src/sdk/BedrockAuth.cpp`）。`locate()` が上記 sig を走査して解決アドレスを保持。`Social_UserImpl_create` が解決しなければ `unsupported`（版ガード）。書き込み・呼び出しなし |
| identity リーダー | `detail::read_account_identity()`。`XboxLiveIdentity +0x30`（XUID）/`+0x1D8`（gamertag）を境界チェック付きで読む純粋関数。XUID が十進でなければ棄却 |
| テスト | `tests/bedrock_auth_tests.cpp`（偽 identity で抽出と不正棄却を検証）、`tests/signature_tests.cpp` にカタログ検証を追加。全 58 CTest 成功 |

`BedrockAuth::current_account()` は未実装。理由: 現在ユーザーへの入口（ClientInstance vtable `+0x598` → UserImpl）の slot 名と concrete 型の一致が未確認（上記「Social::User 相当の配置」末尾）で、誤読の恐れがあるため UI には繋いでいない。識別子リーダー単体は実ゲームなしで検証済み。

### 初回時点のブロッカー

未完了検証 #3 のまま。`RelyingParty=http://xboxlive.com` の XSTS は GameServerToken ではなく、Minecraft auth service 発行の JWT（`cpk` でゲームの署名鍵に紐付く）が必要。自前 Entra アプリでこの token を発行できるか、ゲームの per-session 署名鍵と結び付けられるかは、実ログインでしか確認できず未実施。ここが取れるまで `Use` は無効のまま。

## 追加実装と実測（2026-10-31、v1）

保存済み実アカウントと設定済み自前Entraアプリで、以下の全段HTTP 200と、返されたGameServerTokenの`xid`・`cpk`一致を確認した。

`refresh → Xbox user token → PlayFab用XSTS → PlayFab LoginWithXbox → Minecraft session/start → multiplayer/session/start`

PlayFabプロフィールがXboxInfoを省略する場合があるため、既知のIDがあれば一致を確認し、最終的にはGameServerTokenの必須`xid`を保存済みXUIDと照合する。
Microsoft / PlayFab / Minecraftのトークン、本人ID、秘密鍵はログへ出さない。回転したrefresh tokenはDPAPIで保存し直す。

この結果を受けて`Use`を接続処理へ実装した。`ConnectionAuthInfo::toJson`（RVA `0x46ADD0`）は実際の命令列でも
`RCX=JSON出力領域`、`RDX=ConnectionAuthInfoのconst入力`の2引数と確認済み。入力のToken `+0x10` と認証種別 `+0x08` だけを読む。
DLLのCRTで生成したstd::string自体は渡さず、MSVC配置の借用ビューで元関数にコピーさせる。元のgame-owned objectは変更・破棄しない。
認証種別はFull (`0`)。ConnectionRequest生成関数が使う元の署名鍵はそのまま保持し、Mojangサービスに同じ公開鍵で新しい正規Tokenを発行してもらう。

関数sigに加え、PE timestamp `0x6AB54E37` / image size `0x12C02000` / RVAの一致を必須とした。
未対応版のUseは無効。停止時はauth hookを無効化し、既存callbackカウントがゼロになるまでDLLをアンロードしない。

`UseAccount` → worker認証 → `SessionChanged` → nativeシリアライズ → active XUIDという経路でUIを更新する。
Use押下と認証完了だけでは緑にせず、接続に使ったIDの行を緑にする。接続中に選択を変えたら再接続が必要。

検証済み: 実サービスでのトークン発行、公開鍵とXUIDの一致、借用ビューの短い/長いTokenとFull種別、UIコマンド/状態/緑色表示。
ゲームの新しいフック実行とHiveサーバー側のアカウント表示はユーザーの接続確認待ち。sigカタログの`runtime_validated`はその確認までfalseのままとする。

## NetherNet対応（v2）

v1の実行ログでは14:45にシリアライズまで到達したが、ユーザーからNetherNetエラーと`disconnectionscreen.notauthenticated`が報告された。
後段だけでJWTを変える実装では、それ以前のNetherNet identity assertionは元アカウントの認証のままになり得る。
v2では後段の再発行を廃止し、ゲーム自身の `/api/v1.0/multiplayer/session/start` のHTTP実行直前にAuthorizationを切り替える。

`libHttpClient.GDK.dll`の公開C ABIのexportを対象とする。
引数は`PerformAsync(HCCallHandle, XAsyncBlock*)`、`GetRequestUrl(HCCallHandle, const char**)`、
`SetHeader(HCCallHandle, const char*, const char*, bool allowTracing)`、`GetStatusCode(HCCallHandle, uint32_t*)`。
Authorizationは`allowTracing=false`。正確なHTTPSホスト・パスだけに適用する。任意URL、第三者signaling、本文への書き込みはしない。

NetherNetとLoginPacketの両方がnative処理の結果を使うため、鍵・async所有権・nativeの結果配布は保持される。
シリアライズ時は`xid`一致とTokenの形式・時刻を確認して元のTokenを通す。古いidentityなら接続を拒否して理由を表示し、別JWTを後付けしない。

検証: 実アカウントでXbox / PlayFab / Minecraft sessionを取得し、ゲームの実`libHttpClient.GDK.dll`でnative HTTPを実行。
URLとヘッダーAPIのABI、本文の保持、native callback、HTTP 200、返されたXUIDと公開鍵の一致、後段でTokenを変えないことを確認。
関連7 CTest成功。2026-10-31、v2でNetherNetエラーと`disconnectionscreen.notauthenticated`が消え、
選択アカウントでHiveへ入場できたことをユーザー確認済み。
15:13のゲーム実行ログでnative要求のヘッダー切り替え、HTTP 200、選択アカウントのnative Tokenの使用まで確認。
[実行時検証記録](alt-manager-1.26.52.03-runtime-validation.json)は、利用した3フックと確認範囲だけを記録する。
初回静的解析カタログの他の関数まで実行時検証済みとは扱わない。
