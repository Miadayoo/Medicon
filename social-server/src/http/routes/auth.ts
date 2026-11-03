import type { FastifyInstance } from 'fastify'
import { z } from 'zod'
import { checkKey } from '../../auth/license.ts'
import { ACCOUNT_SECRET_PATTERN, checkAccount, migrateAccount, registerAccount } from '../../auth/account.ts'
import { issueSession } from '../../auth/session.ts'
import { USERNAME_PATTERN } from '../../services/users.ts'
import type { AppContext } from '../../context.ts'
import { clientIp, sendError } from '../common.ts'

const loginBody = z.object({
  key: z.string().min(1).max(64),
  clientVersion: z.string().max(64).optional(),
})

const secret = z.string().length(64).regex(ACCOUNT_SECRET_PATTERN)
const registrationBody = z.strictObject({ username: z.string().regex(USERNAME_PATTERN), secret,
  clientVersion: z.string().max(64).optional() })
const accountBody = z.strictObject({ userId: z.number().int().positive().max(Number.MAX_SAFE_INTEGER), secret,
  clientVersion: z.string().max(64).optional() })
const migrationBody = z.strictObject({ key: z.string().min(1).max(64), secret,
  clientVersion: z.string().max(64).optional() })

export function registerAuthRoutes(app: FastifyInstance, ctx: AppContext): void {
  for (const path of ['/auth/register', '/auth/account', '/auth/migrate'] as const) {
    app.post(path, async (request, reply) => {
      reply.header('Cache-Control', 'no-store')
      const now = ctx.now()
      if (!ctx.limits.auth.tryHit(clientIp(request), now)) return sendError(reply, 429, 'rate_limited')
      const result = (() => {
        if (path === '/auth/register') {
          const body = registrationBody.safeParse(request.body)
          return body.success ? registerAccount(ctx.db, body.data.username, body.data.secret, now) : null
        }
        if (path === '/auth/account') {
          const body = accountBody.safeParse(request.body)
          return body.success ? checkAccount(ctx.db, body.data.userId, body.data.secret) : null
        }
        const body = migrationBody.safeParse(request.body)
        return body.success ? migrateAccount(ctx.db, body.data.key, body.data.secret, now) : null
      })()
      if (!result) return sendError(reply, 400, 'bad_request')
      if (!result.ok) {
        const status = result.error === 'username_taken' ? 409
          : result.error === 'banned' || result.error === 'revoked' ? 403 : 401
        return sendError(reply, status, result.error)
      }
      if (path === '/auth/migrate') ctx.hub.refresh(result.user.id)
      const session = issueSession(ctx.db, result.user.id, now)
      return { ...session, user: result.user }
    })
  }

  // Compatibility for clients that have not migrated yet. Migrated accounts
  // cannot use newly issued legacy keys to bypass their account secret.
  app.post('/auth/license', async (request, reply) => {
    reply.header('Cache-Control', 'no-store')
    const now = ctx.now()
    // Counted before validation so malformed attempts use up the budget too.
    if (!ctx.limits.auth.tryHit(clientIp(request), now)) return sendError(reply, 429, 'rate_limited')
    const body = loginBody.safeParse(request.body)
    if (!body.success) return sendError(reply, 400, 'bad_request')
    const result = checkKey(ctx.db, body.data.key, now)
    if (!result.ok) return sendError(reply, result.error === 'invalid_key' ? 401 : 403, result.error)
    if (ctx.db.prepare('SELECT 1 FROM account_credentials WHERE user_id = ?').get(result.user.id)) {
      return sendError(reply, 401, 'unauthorized')
    }
    const session = issueSession(ctx.db, result.user.id, now)
    return { token: session.token, expiresAt: session.expiresAt, user: result.user }
  })
}
