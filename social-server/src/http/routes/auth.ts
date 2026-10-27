import type { FastifyInstance } from 'fastify'
import { z } from 'zod'
import { checkKey } from '../../auth/license.ts'
import { issueSession } from '../../auth/session.ts'
import type { AppContext } from '../../context.ts'
import { clientIp, sendError } from '../common.ts'

const loginBody = z.object({
  key: z.string().min(1).max(64),
  clientVersion: z.string().max(64).optional(),
})

export function registerAuthRoutes(app: FastifyInstance, ctx: AppContext): void {
  app.post('/auth/license', async (request, reply) => {
    const now = ctx.now()
    // Counted before validation so malformed attempts use up the budget too.
    if (!ctx.limits.auth.tryHit(clientIp(request), now)) return sendError(reply, 429, 'rate_limited')
    const body = loginBody.safeParse(request.body)
    if (!body.success) return sendError(reply, 400, 'bad_request')
    const result = checkKey(ctx.db, body.data.key, now)
    if (!result.ok) return sendError(reply, result.error === 'invalid_key' ? 401 : 403, result.error)
    const session = issueSession(ctx.db, result.user.id, now)
    return { token: session.token, expiresAt: session.expiresAt, user: result.user }
  })
}
