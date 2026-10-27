import type { AddressInfo } from 'node:net'
import { WebSocket } from 'ws'
import { createAdminApp } from '../src/admin/server.ts'
import { createApp } from '../src/app.ts'
import { createContext, type AppContext } from '../src/context.ts'
import { openDb } from '../src/db/db.ts'
import type { ReleaseStore } from '../src/services/releases.ts'

export const ADMIN_SECRET = 'test-admin-secret-0123456789'

// Responses and frames are loosely typed in tests on purpose.
export type Json = any

export interface HttpResult {
  status: number
  body: Json
}

export async function http(
  url: string,
  init: { method?: string; headers?: Record<string, string>; body?: unknown; rawBody?: string } = {},
): Promise<HttpResult> {
  const hasBody = init.body !== undefined || init.rawBody !== undefined
  const response = await fetch(url, {
    method: init.method ?? (hasBody ? 'POST' : 'GET'),
    headers: { ...(hasBody ? { 'Content-Type': 'application/json' } : {}), ...init.headers },
    body: init.rawBody ?? (init.body === undefined ? null : JSON.stringify(init.body)),
  })
  return { status: response.status, body: await response.json().catch(() => null) }
}

export interface TestServer {
  ctx: AppContext
  baseUrl: string
  wsUrl: string
  adminUrl: string
  // Shifts the server's clock (ctx.now) without touching timers.
  clock: { offset: number }
  admin(path: string, body?: unknown): Promise<HttpResult>
  createLicense(name: string): Promise<string>
  login(key: string, ip?: string): Promise<HttpResult>
  // Creates a user and returns a session token for it.
  signUp(name: string): Promise<string>
  close(): Promise<void>
}

export async function startServer(
  options: { idleTimeoutMs?: number; releases?: ReleaseStore | null } = {},
): Promise<TestServer> {
  const clock = { offset: 0 }
  const db = openDb(':memory:')
  const ctx = createContext(db, {
    now: () => Date.now() + clock.offset,
    idleTimeoutMs: options.idleTimeoutMs,
    releases: options.releases ?? null,
  })
  const app = createApp(ctx)
  const adminApp = createAdminApp(ctx, ADMIN_SECRET)
  await app.listen({ host: '127.0.0.1', port: 0 })
  await adminApp.listen({ host: '127.0.0.1', port: 0 })
  const port = (app.server.address() as AddressInfo).port
  const adminUrl = `http://127.0.0.1:${(adminApp.server.address() as AddressInfo).port}`
  // Each login gets its own address so tests don't trip the per-IP sign-in limit by accident.
  let nextIp = 1

  const server: TestServer = {
    ctx,
    baseUrl: `http://127.0.0.1:${port}`,
    wsUrl: `ws://127.0.0.1:${port}/ws`,
    adminUrl,
    clock,
    admin: (path, body) =>
      http(`${adminUrl}${path}`, { method: body === undefined ? 'GET' : 'POST', headers: { 'X-Admin-Secret': ADMIN_SECRET }, body }),
    async createLicense(name) {
      const result = await server.admin('/licenses', { name })
      if (result.status !== 200) throw new Error(`license:create failed: ${result.status}`)
      return result.body.key as string
    },
    login: (key, ip = `10.0.0.${nextIp++}`) =>
      http(`${server.baseUrl}/auth/license`, { headers: { 'CF-Connecting-IP': ip }, body: { key, clientVersion: 'test' } }),
    async signUp(name) {
      const result = await server.login(await server.createLicense(name))
      return result.body.token as string
    },
    async close() {
      await ctx.hub.closeAll(1001, 'test_end', 500)
      await Promise.all([app.close(), adminApp.close()])
      db.close()
    },
  }
  return server
}

export interface TestSocket {
  ws: WebSocket
  closed: Promise<{ code: number; reason: string }>
  send(frame: unknown): void
  // Resolves with the first unread frame that matches.
  next(predicate?: (frame: Json) => boolean, timeoutMs?: number): Promise<Json>
}

export async function connect(url: string, token: string | null): Promise<TestSocket> {
  const ws = new WebSocket(url, token === null ? {} : { headers: { Authorization: `Bearer ${token}` } })
  const inbox: Json[] = []
  const waiters: { predicate: (frame: Json) => boolean; resolve: (frame: Json) => void }[] = []
  ws.on('message', (data) => {
    const frame: Json = JSON.parse(data.toString())
    const index = waiters.findIndex((waiter) => waiter.predicate(frame))
    if (index >= 0) waiters.splice(index, 1)[0]?.resolve(frame)
    else inbox.push(frame)
  })
  const closed = new Promise<{ code: number; reason: string }>((resolve) => {
    ws.on('close', (code, reason) => resolve({ code, reason: reason.toString() }))
  })
  await new Promise<void>((resolve, reject) => {
    ws.once('open', () => resolve())
    ws.once('error', reject)
  })
  ws.on('error', () => {})

  return {
    ws,
    closed,
    send: (frame) => ws.send(typeof frame === 'string' ? frame : JSON.stringify(frame)),
    next(predicate = () => true, timeoutMs = 2000) {
      const index = inbox.findIndex(predicate)
      if (index >= 0) return Promise.resolve(inbox.splice(index, 1)[0])
      return new Promise((resolve, reject) => {
        const waiter = {
          predicate,
          resolve: (frame: Json) => {
            clearTimeout(timer)
            resolve(frame)
          },
        }
        const timer = setTimeout(() => {
          waiters.splice(waiters.indexOf(waiter), 1)
          reject(new Error('timed out waiting for a frame'))
        }, timeoutMs)
        waiters.push(waiter)
      })
    },
  }
}

export function withTimeout<T>(promise: Promise<T>, ms = 2000): Promise<T> {
  return Promise.race([
    promise,
    new Promise<never>((_, reject) => setTimeout(() => reject(new Error(`timed out after ${ms} ms`)), ms)),
  ])
}
