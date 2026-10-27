import type { FastifyError, FastifyInstance, FastifyReply, FastifyRequest } from 'fastify'
import { z } from 'zod'
import { bearerToken, verifyToken } from '../auth/session.ts'
import type { AppContext } from '../context.ts'
import type { ErrorCode } from '../errors.ts'
import { log } from '../log.ts'
import type { Member } from '../services/users.ts'

declare module 'fastify' {
  interface FastifyRequest {
    // Set by requireUser for authenticated routes.
    user: Member | null
  }
}

export const MAX_PAGE_LIMIT = 100

export function sendError(reply: FastifyReply, status: number, error: ErrorCode): FastifyReply {
  return reply.code(status).send({ error })
}

// limit/before of the history endpoints (spec 2.3). A limit above the maximum is clamped.
export function pageQuery(defaultLimit: number) {
  return {
    limit: z.coerce
      .number()
      .int()
      .min(1)
      .default(defaultLimit)
      .transform((limit) => Math.min(limit, MAX_PAGE_LIMIT)),
    before: z.coerce.number().int().positive().optional(),
  }
}

// Keeps every error body in the {"error": code} shape, including Fastify's own
// errors (malformed JSON, oversized bodies, unknown routes).
export function installErrorHandlers(app: FastifyInstance): void {
  app.setErrorHandler((err: FastifyError, _request, reply) => {
    const status = err.statusCode ?? 500
    if (status >= 500) {
      log.error('http error', err)
      return sendError(reply, 500, 'internal')
    }
    return sendError(reply, status, 'bad_request')
  })
  app.setNotFoundHandler((_request, reply) => sendError(reply, 404, 'bad_request'))
}

export function requireUser(ctx: AppContext) {
  return async (request: FastifyRequest, reply: FastifyReply): Promise<FastifyReply | undefined> => {
    const token = bearerToken(request.headers.authorization)
    const session = token === null ? null : verifyToken(ctx.db, token, ctx.now())
    if (!session) return sendError(reply, 401, 'unauthorized')
    request.user = session.user
    return undefined
  }
}

// Through Cloudflare Tunnel every connection comes from 127.0.0.1; the real client is in CF-Connecting-IP.
// The header can be trusted because the server only listens on 127.0.0.1.
export function clientIp(request: FastifyRequest): string {
  const forwarded = request.headers['cf-connecting-ip']
  if (typeof forwarded === 'string' && forwarded.trim() !== '') return forwarded.trim()
  return request.socket.remoteAddress ?? 'unknown'
}
