import type { FastifyInstance } from 'fastify'
import { z } from 'zod'
import type { AppContext } from '../../context.ts'
import { listNews } from '../../services/news.ts'
import { pageQuery, requireUser, sendError } from '../common.ts'

const newsQuery = z.object({
  kind: z.enum(['news', 'changelog']).default('news'),
  ...pageQuery(20),
})

export function registerNewsRoutes(app: FastifyInstance, ctx: AppContext): void {
  app.get('/news', { preHandler: requireUser(ctx) }, async (request, reply) => {
    const query = newsQuery.safeParse(request.query)
    if (!query.success) return sendError(reply, 400, 'bad_request')
    return { items: listNews(ctx.db, query.data) }
  })
}
