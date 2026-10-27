import { STATUS_CODES, type IncomingMessage } from 'node:http'
import type { Duplex } from 'node:stream'
import Fastify, { type FastifyInstance } from 'fastify'
import { WebSocketServer } from 'ws'
import { bearerToken, verifyToken } from './auth/session.ts'
import type { AppContext } from './context.ts'
import type { ErrorCode } from './errors.ts'
import { installErrorHandlers } from './http/common.ts'
import { registerAuthRoutes } from './http/routes/auth.ts'
import { registerChatRoutes } from './http/routes/chat.ts'
import { registerNewsRoutes } from './http/routes/news.ts'

// Requests and frames are small: a message body is at most 300 code points.
const MAX_PAYLOAD_BYTES = 16 * 1024

// The public server (REST + /ws). Cloudflare Tunnel forwards social.example.com here.
export function createApp(ctx: AppContext): FastifyInstance {
  const app = Fastify({ logger: false, bodyLimit: MAX_PAYLOAD_BYTES })
  app.decorateRequest('user', null)
  installErrorHandlers(app)
  registerAuthRoutes(app, ctx)
  registerNewsRoutes(app, ctx)
  registerChatRoutes(app, ctx)

  // ws shares Fastify's HTTP server. The token is checked before the upgrade
  // completes, so an invalid one gets a plain HTTP 401 (spec 2.4).
  const wss = new WebSocketServer({ noServer: true, clientTracking: false, maxPayload: MAX_PAYLOAD_BYTES })
  app.server.on('upgrade', (request: IncomingMessage, socket: Duplex, head: Buffer) => {
    socket.on('error', () => socket.destroy())
    const { pathname } = new URL(request.url ?? '/', 'http://localhost')
    if (pathname !== '/ws') return rejectUpgrade(socket, 404, 'bad_request')
    const token = bearerToken(request.headers.authorization)
    const session = token === null ? null : verifyToken(ctx.db, token, ctx.now())
    if (!session) return rejectUpgrade(socket, 401, 'unauthorized')
    wss.handleUpgrade(request, socket, head, (ws) => ctx.hub.attach(ws, session))
  })
  return app
}

function rejectUpgrade(socket: Duplex, status: number, error: ErrorCode): void {
  const body = JSON.stringify({ error })
  socket.end(
    `HTTP/1.1 ${status} ${STATUS_CODES[status] ?? ''}\r\n` +
      'Content-Type: application/json\r\n' +
      `Content-Length: ${Buffer.byteLength(body)}\r\n` +
      'Connection: close\r\n\r\n' +
      body,
  )
}
