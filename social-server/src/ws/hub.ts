import { WebSocket, type RawData } from 'ws'
import type { Session } from '../auth/session.ts'
import { log } from '../log.ts'
import type { Member, UserRef } from '../services/users.ts'
import {
  CLOSE,
  clientMessage,
  requestId,
  type ClientMessage,
  type KickReason,
  type RequestMessage,
  type ServerMessage,
} from './protocol.ts'

export interface HubOptions {
  now: () => number
  idleTimeoutMs: number
}

// What the hub hands to the services (ws/handlers.ts).
export interface HubEvents {
  // A validated request from `user`; `reply` answers on that user's socket.
  request(user: Member, message: RequestMessage, reply: (frame: ServerMessage) => void): void
  // The user came online (first connection) or went offline (last one closed).
  presence(user: UserRef, online: boolean): void
}

interface Connection {
  ws: WebSocket
  user: Member
  idleTimer: NodeJS.Timeout
  expiryTimer: NodeJS.Timeout
}

// setTimeout fires immediately for longer delays.
const MAX_TIMER_MS = 2 ** 31 - 1

// Owns the single live connection of each user (spec 2.4) and routes frames.
export class Hub {
  private readonly options: HubOptions
  private readonly connections = new Map<number, Connection>()
  // Every open socket, including replaced or kicked ones still finishing their close handshake.
  private readonly sockets = new Set<WebSocket>()
  private events: HubEvents | null = null

  constructor(options: HubOptions) {
    this.options = options
  }

  setEvents(events: HubEvents): void {
    this.events = events
  }

  get size(): number {
    return this.connections.size
  }

  isOnline(userId: number): boolean {
    return this.connections.has(userId)
  }

  attach(ws: WebSocket, session: Session): void {
    const conn: Connection = {
      ws,
      user: session.user,
      // No frame for this long means the peer is gone, possibly without a TCP close.
      idleTimer: setTimeout(() => ws.terminate(), this.options.idleTimeoutMs),
      // 4001 makes the client sign in again with its stored key before reconnecting.
      expiryTimer: setTimeout(
        () => ws.close(CLOSE.tokenInvalid, 'token_expired'),
        Math.min(MAX_TIMER_MS, Math.max(0, session.expiresAt - this.options.now())),
      ),
    }
    this.sockets.add(ws)
    const previous = this.connections.get(conn.user.id)
    this.connections.set(conn.user.id, conn)
    if (previous) {
      previous.ws.close(CLOSE.replaced, 'replaced')
    } else {
      log.info(`ws connections=${this.connections.size}`)
      this.events?.presence(conn.user, true)
    }

    const alive = (): void => {
      conn.idleTimer.refresh()
    }
    ws.on('message', (data, isBinary) => {
      alive()
      this.onMessage(conn, data, isBinary)
    })
    ws.on('ping', alive)
    ws.on('pong', alive)
    ws.on('error', (err) => log.error('ws error', err))
    ws.on('close', () => {
      clearTimeout(conn.idleTimer)
      clearTimeout(conn.expiryTimer)
      this.sockets.delete(ws)
      this.release(conn)
    })

    this.sendTo(conn, { t: 'hello', user: conn.user, serverTime: this.options.now() })
  }

  send(userId: number, message: ServerMessage): boolean {
    const conn = this.connections.get(userId)
    if (!conn) return false
    this.sendTo(conn, message)
    return true
  }

  broadcast(message: ServerMessage): void {
    const data = JSON.stringify(message)
    for (const conn of this.connections.values()) {
      if (conn.ws.readyState === WebSocket.OPEN) conn.ws.send(data)
    }
  }

  // The user's rank changed: 4001 makes the client sign in again and get a hello
  // (and permissions) with the new rank.
  refresh(userId: number): boolean {
    const conn = this.connections.get(userId)
    if (!conn) return false
    conn.ws.close(CLOSE.tokenInvalid, 'rank_changed')
    return true
  }

  // The client shows the reason and does not reconnect after 4003 (spec 2.6).
  kick(userId: number, reason: KickReason): boolean {
    const conn = this.connections.get(userId)
    if (!conn) return false
    this.sendTo(conn, { t: 'kicked', reason })
    conn.ws.close(CLOSE.kicked, reason)
    this.release(conn)
    return true
  }

  // Waits for every close handshake, terminating sockets that don't answer in time.
  async closeAll(code: number, reason: string, timeoutMs = 3000): Promise<void> {
    await Promise.all(
      [...this.sockets].map(
        (ws) =>
          new Promise<void>((resolve) => {
            if (ws.readyState === WebSocket.CLOSED) return resolve()
            const timer = setTimeout(() => ws.terminate(), timeoutMs)
            ws.once('close', () => {
              clearTimeout(timer)
              resolve()
            })
            ws.close(code, reason)
          }),
      ),
    )
  }

  private release(conn: Connection): void {
    if (this.connections.get(conn.user.id) !== conn) return
    this.connections.delete(conn.user.id)
    log.info(`ws connections=${this.connections.size}`)
    this.events?.presence(conn.user, false)
  }

  private onMessage(conn: Connection, data: RawData, isBinary: boolean): void {
    // A replaced or kicked socket may still deliver frames while it closes; it no longer speaks for the user.
    if (this.connections.get(conn.user.id) !== conn || isBinary) return
    let raw: unknown
    try {
      raw = JSON.parse(data.toString())
    } catch {
      return
    }
    const parsed = clientMessage.safeParse(raw)
    if (parsed.success) return this.dispatch(conn, parsed.data)
    // Spec 3.5: answer malformed requests only when they carry an id to answer to.
    const id = nonceOf(raw)
    if (id !== null) this.sendTo(conn, { t: 'ack', id, ok: false, error: 'bad_request' })
  }

  private dispatch(conn: Connection, message: ClientMessage): void {
    if (message.t === 'ping') return this.sendTo(conn, { t: 'pong' })
    try {
      this.events?.request(conn.user, message, (frame) => this.sendTo(conn, frame))
    } catch (err) {
      log.error('ws request failed', err)
      if ('id' in message) this.sendTo(conn, { t: 'ack', id: message.id, ok: false, error: 'internal' })
    }
  }

  private sendTo(conn: Connection, message: ServerMessage): void {
    if (conn.ws.readyState === WebSocket.OPEN) conn.ws.send(JSON.stringify(message))
  }
}

function nonceOf(raw: unknown): string | null {
  if (typeof raw !== 'object' || raw === null || !('id' in raw)) return null
  const id = requestId.safeParse(raw.id)
  return id.success ? id.data : null
}
