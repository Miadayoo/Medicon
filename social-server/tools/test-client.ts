import { createInterface, type Interface } from 'node:readline'
import { parseArgs } from 'node:util'
import { WebSocket } from 'ws'

// A command-line client for checking the server without the DLL.

const USAGE = `使い方: npm run test-client -- [--server <url>] [--key <key>]

  --server  サーバーのURL（既定: http://127.0.0.1:8787）
  --key     ライセンスキー。省略すると環境変数 MEDICON_KEY を使い、それもなければ入力を求めます`

const COMMANDS = `コマンド:
  /news [news|changelog]   お知らせ（または Change Log）の最新20件を取得
  /g <本文>                Global に投稿
  /global                  Global の最新20件
  /friends                 フレンドと申請の一覧
  /search <前方一致>        ユーザーを検索
  /add <name>              フレンド申請
  /accept <userId>         届いた申請を承認（/decline で拒否）
  /remove <userId>         フレンド削除、または送った申請の取消
  /dm <userId> <本文>       DM を送る
  /dmh <userId>            DM の最新20件
  /read <userId> <msgId>   DM を msgId まで既読にする
  /ping                    ping を送る（30秒ごとに自動でも送ります）
  /raw <text>              任意のテキストフレームをそのまま送る
  /help                    この一覧
  /quit                    終了`

const PING_INTERVAL_MS = 30_000

const CLOSE_MEANINGS: Record<number, string> = {
  1001: 'サーバー停止',
  1006: '通信断',
  4001: 'トークン無効・期限切れ',
  4003: 'revoke または BAN',
  4009: '別の場所で接続されました',
}

interface LoginResponse {
  token: string
  expiresAt: number
  user: { id: number; name: string }
}

interface NewsItem {
  id: number
  kind: string
  title: string
  version: string | null
  createdAt: number
}

async function main(): Promise<void> {
  const { values } = parseArgs({
    options: {
      server: { type: 'string', default: 'http://127.0.0.1:8787' },
      key: { type: 'string' },
      help: { type: 'boolean', short: 'h' },
    },
  })
  if (values.help === true) {
    console.log(`${USAGE}\n\n${COMMANDS}`)
    return
  }
  const server = values.server.replace(/\/+$/, '')
  const rl = createInterface({ input: process.stdin, output: process.stdout })
  rl.on('SIGINT', () => rl.close())

  const key = values.key ?? process.env.MEDICON_KEY ?? (await ask(rl, 'ライセンスキー: '))
  const login = await signIn(server, key)
  if (login === null) {
    rl.close()
    process.exitCode = 1
    return
  }
  print(`ログインしました: ${login.user.name} (id=${login.user.id})`)

  const ws = new WebSocket(`${server.replace(/^http/, 'ws')}/ws`, {
    headers: { Authorization: `Bearer ${login.token}` },
  })
  const pingTimer = setInterval(() => send(ws, JSON.stringify({ t: 'ping' })), PING_INTERVAL_MS)

  ws.on('open', () => print(`WebSocketに接続しました。${COMMANDS}`))
  ws.on('message', (data) => print(`< ${data.toString()}`))
  ws.on('error', (err) => print(`WebSocketエラー: ${err.message}`))
  ws.on('close', (code, reason) => {
    clearInterval(pingTimer)
    const meaning = CLOSE_MEANINGS[code]
    print(`切断されました: code=${code}${reason.length > 0 ? ` reason=${reason.toString()}` : ''}${meaning ? `（${meaning}）` : ''}`)
    rl.close()
  })

  rl.on('line', (line) => {
    handleCommand(line.trim(), server, login.token, ws, rl).catch((err: unknown) => print(`エラー: ${String(err)}`))
  })
  rl.on('close', () => {
    if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) ws.close(1000)
  })
}

async function handleCommand(line: string, server: string, token: string, ws: WebSocket, rl: Interface): Promise<void> {
  if (line === '') return
  const command = line.split(/\s+/, 1)[0] ?? ''
  const arg = line.slice(command.length).trim()
  switch (command) {
    case '/news':
      await showNews(server, token, arg === '' ? 'news' : arg)
      break
    case '/ping':
      send(ws, JSON.stringify({ t: 'ping' }))
      break
    case '/g':
      send(ws, JSON.stringify({ t: 'global.send', id: nonce(), body: arg }))
      break
    case '/global':
      await showMessages(server, token, '/global/messages?limit=20')
      break
    case '/friends':
      await showJson(server, token, '/friends')
      break
    case '/search':
      await showJson(server, token, `/users/search?q=${encodeURIComponent(arg)}`)
      break
    case '/add':
      send(ws, JSON.stringify({ t: 'friend.request', id: nonce(), name: arg }))
      break
    case '/accept':
    case '/decline':
      send(ws, JSON.stringify({ t: 'friend.respond', id: nonce(), userId: Number(arg), accept: command === '/accept' }))
      break
    case '/remove':
      send(ws, JSON.stringify({ t: 'friend.remove', id: nonce(), userId: Number(arg) }))
      break
    case '/dm': {
      const [to = '', ...rest] = arg.split(/\s+/)
      send(ws, JSON.stringify({ t: 'dm.send', id: nonce(), to: Number(to), body: rest.join(' ') }))
      break
    }
    case '/dmh':
      await showMessages(server, token, `/dm/${encodeURIComponent(arg)}/messages?limit=20`)
      break
    case '/read': {
      const [other = '', upTo = ''] = arg.split(/\s+/)
      send(ws, JSON.stringify({ t: 'dm.read', with: Number(other), upTo: Number(upTo) }))
      break
    }
    case '/raw':
      send(ws, arg)
      break
    case '/help':
      print(COMMANDS)
      break
    case '/quit':
      rl.close()
      break
    default:
      print('不明なコマンドです。/help で一覧を表示します')
  }
}

async function signIn(server: string, key: string): Promise<LoginResponse | null> {
  let response: Response
  try {
    response = await fetch(`${server}/auth/license`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ key, clientVersion: 'test-client' }),
    })
  } catch {
    print(`サーバーに接続できません: ${server}`)
    return null
  }
  const data: unknown = await response.json().catch(() => null)
  if (!response.ok) {
    print(`ログインに失敗しました: HTTP ${response.status} ${errorCode(data)}`)
    return null
  }
  return data as LoginResponse
}

async function showNews(server: string, token: string, kind: string): Promise<void> {
  const response = await fetch(`${server}/news?kind=${encodeURIComponent(kind)}&limit=20`, {
    headers: { Authorization: `Bearer ${token}` },
  })
  const data: unknown = await response.json().catch(() => null)
  if (!response.ok) {
    print(`取得に失敗しました: HTTP ${response.status} ${errorCode(data)}`)
    return
  }
  const items = (data as { items: NewsItem[] }).items
  if (items.length === 0) print('（まだありません）')
  for (const item of items) {
    const version = item.version === null ? '' : ` ${item.version}`
    print(`#${item.id} [${item.kind}${version}] ${item.title} (${new Date(item.createdAt).toLocaleString()})`)
  }
}

interface Message {
  id: number
  author: { id: number; name: string }
  body: string
  createdAt: number
}

async function getJson(server: string, token: string, path: string): Promise<unknown> {
  const response = await fetch(`${server}${path}`, { headers: { Authorization: `Bearer ${token}` } })
  const data: unknown = await response.json().catch(() => null)
  if (!response.ok) {
    print(`取得に失敗しました: HTTP ${response.status} ${errorCode(data)}`)
    return null
  }
  return data
}

async function showJson(server: string, token: string, path: string): Promise<void> {
  const data = await getJson(server, token, path)
  if (data !== null) print(JSON.stringify(data))
}

async function showMessages(server: string, token: string, path: string): Promise<void> {
  const data = (await getJson(server, token, path)) as { items: Message[] } | null
  if (data === null) return
  if (data.items.length === 0) print('（まだありません）')
  // Oldest first, as a chat reads.
  for (const m of [...data.items].reverse()) {
    print(`#${m.id} ${m.author.name}: ${m.body} (${new Date(m.createdAt).toLocaleTimeString()})`)
  }
}

let nextNonce = 1
function nonce(): string {
  return `c${nextNonce++}`
}

function send(ws: WebSocket, text: string): void {
  if (ws.readyState !== WebSocket.OPEN) {
    print('接続していません')
    return
  }
  ws.send(text)
  print(`> ${text}`)
}

function errorCode(data: unknown): string {
  return typeof data === 'object' && data !== null && 'error' in data ? String(data.error) : ''
}

function ask(rl: Interface, prompt: string): Promise<string> {
  return new Promise((resolve) => rl.question(prompt, (answer) => resolve(answer.trim())))
}

function print(message: string): void {
  console.log(`[${new Date().toLocaleTimeString()}] ${message}`)
}

await main()
