import { createHash, timingSafeEqual } from 'node:crypto'
import Fastify, { type FastifyInstance } from 'fastify'
import { z } from 'zod'
import { addLicense, listLicenses, revokeLicenses } from '../auth/license.ts'
import { deleteUserSessions } from '../auth/session.ts'
import type { AppContext } from '../context.ts'
import type { Db } from '../db/db.ts'
import type { ErrorCode } from '../errors.ts'
import { installErrorHandlers, sendError } from '../http/common.ts'
import { publishRelease, RELEASE_BODY_LIMIT, registerReleaseParser } from '../http/routes/releases.ts'
import { createNews, newsInput } from '../services/news.ts'
import { parseReleaseUpload } from '../services/releases.ts'
import { createUser, findUserByName, setBanned, setRank, USERNAME_PATTERN, type User, type UserRef } from '../services/users.ts'

const nameBody = z.object({ name: z.string().regex(USERNAME_PATTERN) })

const newsBody = newsInput
const rankBody = z.object({ name: z.string().regex(USERNAME_PATTERN), rank: z.enum(['user', 'owner']) })

// Admin API for admin/cli.ts. It listens on 127.0.0.1 only and the tunnel never
// forwards to it. The CLI calls this instead of writing the DB so that kicks and
// broadcasts reach connected clients immediately.
export function createAdminApp(ctx: AppContext, secret: string): FastifyInstance {
  const app = Fastify({ logger: false, bodyLimit: 256 * 1024 })
  installErrorHandlers(app)

  const expected = digest(secret)
  app.addHook('onRequest', async (request, reply) => {
    const given = request.headers['x-admin-secret']
    // Digests have a fixed length, so timingSafeEqual never sees mismatched sizes.
    if (typeof given !== 'string' || !timingSafeEqual(digest(given), expected)) {
      return sendError(reply, 401, 'unauthorized')
    }
    return undefined
  })

  // Creates the user when the name is new, otherwise adds another key to it.
  app.post('/licenses', async (request, reply) => {
    const body = nameBody.safeParse(request.body)
    if (!body.success) return sendError(reply, 400, 'bad_request')
    const now = ctx.now()
    return ctx.db.transaction(() => {
      const existing = findUserByName(ctx.db, body.data.name)
      const user = existing ?? createUser(ctx.db, body.data.name, now)
      return { user: toRef(user), createdUser: existing === undefined, key: addLicense(ctx.db, user.id, now) }
    })()
  })

  app.get('/licenses', async () => ({ items: listLicenses(ctx.db) }))

  app.post('/licenses/revoke', async (request, reply) => {
    const found = lookupUser(ctx.db, request.body)
    if (!found.ok) return sendError(reply, found.status, found.error)
    const { user } = found
    const revokedKeys = ctx.db.transaction(() => {
      deleteUserSessions(ctx.db, user.id)
      return revokeLicenses(ctx.db, user.id)
    })()
    return { user: toRef(user), revokedKeys, kicked: ctx.hub.kick(user.id, 'revoked') }
  })

  // Keys stay as they are, so an unbanned user can sign in with the same key again.
  app.post('/users/ban', async (request, reply) => {
    const found = lookupUser(ctx.db, request.body)
    if (!found.ok) return sendError(reply, found.status, found.error)
    const { user } = found
    ctx.db.transaction(() => {
      setBanned(ctx.db, user.id, true)
      deleteUserSessions(ctx.db, user.id)
    })()
    return { user: toRef(user), kicked: ctx.hub.kick(user.id, 'banned') }
  })

  app.post('/users/unban', async (request, reply) => {
    const found = lookupUser(ctx.db, request.body)
    if (!found.ok) return sendError(reply, found.status, found.error)
    setBanned(ctx.db, found.user.id, false)
    return { user: toRef(found.user) }
  })

  // Owners post News / Change Log from the client. A connected user signs in
  // again at once so the new rank applies.
  app.post('/users/rank', async (request, reply) => {
    const body = rankBody.safeParse(request.body)
    if (!body.success) return sendError(reply, 400, 'bad_request')
    const user = findUserByName(ctx.db, body.data.name)
    if (!user) return sendError(reply, 404, 'user_not_found')
    setRank(ctx.db, user.id, body.data.rank)
    return { user: toRef(user), rank: body.data.rank, reconnected: ctx.hub.refresh(user.id) }
  })

  app.post('/news', async (request, reply) => {
    const body = newsBody.safeParse(request.body)
    if (!body.success) return sendError(reply, 400, 'bad_request')
    const item = createNews(ctx.db, body.data, ctx.now())
    ctx.hub.broadcast({ t: 'news.new', item })
    return { item }
  })

  // release:publish from the CLI: the same one-step DLL + Change Log as owners.
  registerReleaseParser(app)
  app.post('/releases', { bodyLimit: RELEASE_BODY_LIMIT }, async (request, reply) => {
    if (!Buffer.isBuffer(request.body)) return sendError(reply, 400, 'bad_release')
    const upload = parseReleaseUpload(request.body)
    if (!upload) return sendError(reply, 400, 'bad_release')
    const outcome = await publishRelease(ctx, upload.metadata, upload.dll)
    if (!outcome.ok) return sendError(reply, outcome.status, outcome.error)
    return { manifest: outcome.manifest, item: outcome.item }
  })

  return app
}

type Lookup = { ok: true; user: User } | { ok: false; status: number; error: ErrorCode }

function lookupUser(db: Db, body: unknown): Lookup {
  const parsed = nameBody.safeParse(body)
  if (!parsed.success) return { ok: false, status: 400, error: 'bad_request' }
  const user = findUserByName(db, parsed.data.name)
  return user ? { ok: true, user } : { ok: false, status: 404, error: 'user_not_found' }
}

function toRef(user: UserRef): UserRef {
  return { id: user.id, name: user.name }
}

function digest(value: string): Buffer {
  return createHash('sha256').update(value, 'utf8').digest()
}
