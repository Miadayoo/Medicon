import type { FastifyInstance } from 'fastify'
import { z } from 'zod'
import type { AppContext } from '../../context.ts'
import { areFriends, listFriends, searchUsers } from '../../services/friends.ts'
import { listDms, listGlobalMessages } from '../../services/messages.ts'
import { pageQuery, requireUser, sendError } from '../common.ts'

const messagesQuery = z.object(pageQuery(50))
const dmParams = z.object({ userId: z.coerce.number().int().positive() })
const searchQuery = z.object({ q: z.string().trim().min(1).max(16) })

// Spec 2.3: Global and DM history, the friend list and user search.
export function registerChatRoutes(app: FastifyInstance, ctx: AppContext): void {
  const auth = { preHandler: requireUser(ctx) }

  app.get('/global/messages', auth, async (request, reply) => {
    const query = messagesQuery.safeParse(request.query)
    if (!query.success) return sendError(reply, 400, 'bad_request')
    return { items: listGlobalMessages(ctx.db, query.data) }
  })

  app.get('/dm/:userId/messages', auth, async (request, reply) => {
    const params = dmParams.safeParse(request.params)
    const query = messagesQuery.safeParse(request.query)
    if (!params.success || !query.success) return sendError(reply, 400, 'bad_request')
    const me = request.user!.id
    if (!areFriends(ctx.db, me, params.data.userId)) return sendError(reply, 403, 'not_friends')
    return { items: listDms(ctx.db, me, params.data.userId, query.data) }
  })

  app.get('/friends', auth, async (request) => listFriends(ctx.db, request.user!.id, (id) => ctx.hub.isOnline(id)))

  app.get('/users/search', auth, async (request, reply) => {
    const query = searchQuery.safeParse(request.query)
    if (!query.success) return sendError(reply, 400, 'bad_request')
    return { items: searchUsers(ctx.db, request.user!.id, query.data.q) }
  })
}
