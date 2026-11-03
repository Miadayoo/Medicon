import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { parseArgs } from 'node:util'
import { loadConfig, loadEnvFile } from '../src/config.ts'
import { buildReleaseUpload, RELEASE_CONTENT_TYPE } from '../src/services/releases.ts'

const USAGE = `使い方: npm run admin -- <コマンド> [オプション]

  license:create --name <name>   ユーザーを作成（既にいればキーだけ追加）してキーを発行
  license:revoke --name <name>   そのユーザーの全キーを無効化し、接続中なら切断
  license:list                   ユーザーとキーの一覧
  user:list                      全ユーザーの ID・名前・ランクを一覧表示
  user:ban --id <id>             BANして、接続中なら切断
  user:unban --id <id>           BANを解除
  user:rank --id <id> --rank <owner|user>
                                 ランクを変更（owner は Social の Admin タブから投稿できる）
                                 --id の代わりに --name も利用可能
  news:post --kind <news|changelog> --title <title> --body-file <path> [--version <v>]
                                 お知らせ／Change Logを投稿して全接続に配信
  release:publish --file <MediconCore.dll> --version <x.y.z> --title <title> --body-file <path>
                                 Core DLL を署名付きで公開し、同じ内容の Change Log を投稿`

// Errors meant for the operator: printed without a stack trace.
class CliError extends Error {}

interface UserRef {
  id: number
  name: string
}

interface LicenseRow {
  name: string
  banned: boolean
  licenseId: number
  status: string
  createdAt: number
  lastUsedAt: number | null
}

async function main(): Promise<void> {
  const { positionals, values } = parseArgs({
    allowPositionals: true,
    options: {
      name: { type: 'string' },
      id: { type: 'string' },
      kind: { type: 'string' },
      rank: { type: 'string' },
      title: { type: 'string' },
      'body-file': { type: 'string' },
      file: { type: 'string' },
      version: { type: 'string' },
      help: { type: 'boolean', short: 'h' },
    },
  })
  const command = positionals[0]
  if (values.help === true || command === undefined) {
    console.log(USAGE)
    return
  }

  loadEnvFile()
  const config = loadConfig()
  const admin = adminClient(config.adminPort, config.adminSecret)

  switch (command) {
    case 'user:list': {
      const result = await admin.get<{ items: (UserRef & { rank: string; banned: number; hasAccount: number })[] }>('/users')
      printTable(['id', 'name', 'rank', 'ban', 'auth'], result.items.map((user) => [
        String(user.id), user.name, user.rank, user.banned ? 'BAN' : '-', user.hasAccount ? 'account' : 'license',
      ]))
      break
    }
    case 'license:create': {
      const result = await admin.post<{ user: UserRef; createdUser: boolean; key: string }>('/licenses', {
        name: required(values.name, '--name'),
      })
      console.log(
        result.createdUser
          ? `ユーザー ${result.user.name} (id=${result.user.id}) を作成しました`
          : `既存のユーザー ${result.user.name} (id=${result.user.id}) にキーを追加しました`,
      )
      console.log(`ライセンスキー: ${result.key}`)
      console.log('このキーは今回しか表示されません。控えてから本人に渡してください。')
      break
    }
    case 'license:revoke': {
      const result = await admin.post<{ user: UserRef; revokedKeys: number; kicked: boolean }>('/licenses/revoke', userSelector(values))
      console.log(`${result.user.name} のキーを ${result.revokedKeys} 件無効化しました${result.kicked ? '（接続を切断）' : ''}`)
      break
    }
    case 'license:list': {
      const result = await admin.get<{ items: LicenseRow[] }>('/licenses')
      if (result.items.length === 0) {
        console.log('キーはまだありません')
        break
      }
      printTable(
        ['name', 'ban', 'key', 'status', 'created', 'last_used'],
        result.items.map((row) => [
          row.name,
          row.banned ? 'BAN' : '-',
          `#${row.licenseId}`,
          row.status,
          formatTime(row.createdAt),
          row.lastUsedAt === null ? '-' : formatTime(row.lastUsedAt),
        ]),
      )
      break
    }
    case 'user:ban': {
      const result = await admin.post<{ user: UserRef; kicked: boolean }>('/users/ban', userSelector(values))
      console.log(`${result.user.name} をBANしました${result.kicked ? '（接続を切断）' : ''}`)
      break
    }
    case 'user:unban': {
      const result = await admin.post<{ user: UserRef }>('/users/unban', userSelector(values))
      console.log(`${result.user.name} のBANを解除しました`)
      break
    }
    case 'user:rank': {
      const rank = values.rank
      if (rank !== 'owner' && rank !== 'user') throw new CliError('--rank には owner か user を指定してください')
      const result = await admin.post<{ user: UserRef; reconnected: boolean }>('/users/rank', {
        ...userSelector(values),
        rank,
      })
      console.log(`${result.user.name} のランクを ${rank} にしました${result.reconnected ? '（接続中のため再接続させました）' : ''}`)
      break
    }
    case 'news:post': {
      const kind = values.kind
      if (kind !== 'news' && kind !== 'changelog') throw new CliError('--kind には news か changelog を指定してください')
      const title = required(values.title, '--title')
      const body = readBody(required(values['body-file'], '--body-file'))
      const result = await admin.post<{ item: { id: number } }>('/news', { kind, title, body, version: values.version })
      console.log(`${kind === 'news' ? 'お知らせ' : 'Change Log'}を投稿しました (id=${result.item.id})`)
      break
    }
    case 'release:publish': {
      const file = resolve(process.env.INIT_CWD ?? process.cwd(), required(values.file, '--file'))
      let dll: Buffer
      try {
        dll = readFileSync(file)
      } catch {
        throw new CliError(`DLL を読めません: ${file}`)
      }
      const upload = buildReleaseUpload(
        {
          version: required(values.version, '--version'),
          title: required(values.title, '--title'),
          body: readBody(required(values['body-file'], '--body-file')),
        },
        dll,
      )
      const result = await admin.raw<{ manifest: { version: string; sha256: string; size: number } }>(
        '/releases',
        upload,
        RELEASE_CONTENT_TYPE,
      )
      console.log(`Core ${result.manifest.version} を公開しました (${result.manifest.size} bytes, sha256 ${result.manifest.sha256})`)
      break
    }
    default:
      throw new CliError(`不明なコマンドです: ${command}\n\n${USAGE}`)
  }
}

function userSelector(values: { id?: string; name?: string }): { id: number } | { name: string } {
  if (values.id !== undefined) {
    if (values.name !== undefined || !/^[1-9][0-9]*$/.test(values.id) || !Number.isSafeInteger(Number(values.id))) {
      throw new CliError('--id には正の整数を指定し、--name と併用しないでください')
    }
    return { id: Number(values.id) }
  }
  return { name: required(values.name, '--id または --name') }
}

function required(value: string | undefined, flag: string): string {
  if (value === undefined || value.trim() === '') throw new CliError(`${flag} を指定してください`)
  return value
}

function readBody(path: string): string {
  // npm runs scripts in the package directory; INIT_CWD is where the command was typed.
  const fullPath = resolve(process.env.INIT_CWD ?? process.cwd(), path)
  try {
    return readFileSync(fullPath, 'utf8').replace(/^﻿/, '')
  } catch {
    throw new CliError(`本文ファイルを読めません: ${fullPath}`)
  }
}

function adminClient(port: number, secret: string) {
  const request = async <T>(method: 'GET' | 'POST', path: string, body?: unknown, raw?: { bytes: Buffer; type: string }): Promise<T> => {
    let response: Response
    try {
      response = await fetch(`http://127.0.0.1:${port}${path}`, {
        method,
        headers: raw
          ? { 'X-Admin-Secret': secret, 'Content-Type': raw.type }
          : body === undefined
            ? { 'X-Admin-Secret': secret }
            : { 'X-Admin-Secret': secret, 'Content-Type': 'application/json' },
        body: raw ? new Uint8Array(raw.bytes) : body === undefined ? null : JSON.stringify(body),
      })
    } catch {
      throw new CliError(`管理ポート 127.0.0.1:${port} に接続できません。サーバーが起動しているか確認してください。`)
    }
    const data: unknown = await response.json().catch(() => null)
    if (!response.ok) throw new CliError(describeError(response.status, data))
    return data as T
  }
  return {
    get: <T>(path: string) => request<T>('GET', path),
    post: <T>(path: string, body: unknown) => request<T>('POST', path, body),
    raw: <T>(path: string, bytes: Buffer, type: string) => request<T>('POST', path, undefined, { bytes, type }),
  }
}

function describeError(status: number, data: unknown): string {
  const code = typeof data === 'object' && data !== null && 'error' in data ? String(data.error) : ''
  switch (code) {
    case 'unauthorized':
      return 'ADMIN_SECRET がサーバーと一致しません。.env を確認してください。'
    case 'user_not_found':
      return 'そのユーザーは存在しません。'
    case 'bad_request':
      return '入力が不正です。usernameは3〜16文字の英数字と _ です。'
    case 'releases_disabled':
      return 'Core の公開が無効です。.env の RELEASE_SIGNING_KEY と PUBLIC_BASE_URL を設定して再起動してください。'
    case 'version_conflict':
      return 'そのバージョンは公開済みか、最新より古いです。最新より大きい x.y.z を指定してください。'
    case 'bad_release':
      return 'DLL かバージョンが不正です。x64 の DLL と x.y.z 形式のバージョンを指定してください。'
    default:
      return `サーバーエラー: HTTP ${status} ${code}`
  }
}

function formatTime(ms: number): string {
  const date = new Date(ms)
  const pad = (value: number): string => String(value).padStart(2, '0')
  return (
    `${date.getFullYear()}/${pad(date.getMonth() + 1)}/${pad(date.getDate())} ` +
    `${pad(date.getHours())}:${pad(date.getMinutes())}`
  )
}

function printTable(header: string[], rows: string[][]): void {
  const widths = header.map((title, i) => Math.max(title.length, ...rows.map((row) => row[i]?.length ?? 0)))
  const format = (cells: string[]): string => cells.map((cell, i) => cell.padEnd(widths[i] ?? 0)).join('  ').trimEnd()
  console.log(format(header))
  for (const row of rows) console.log(format(row))
}

main().catch((err: unknown) => {
  console.error(err instanceof CliError ? err.message : err)
  process.exitCode = 1
})
