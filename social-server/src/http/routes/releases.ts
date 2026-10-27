import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify'
import type { AppContext } from '../../context.ts'
import { log } from '../../log.ts'
import { createNews, type NewsItem } from '../../services/news.ts'
import {
  CORE_FILE,
  MAX_CORE_BYTES,
  RELEASE_CONTENT_TYPE,
  VERSION_PATTERN,
  changelogLines,
  parseReleaseUpload,
  type Manifest,
  type PublishError,
  type ReleaseMetadata,
} from '../../services/releases.ts'
import { requireUser, sendError } from '../common.ts'

export const RELEASE_BODY_LIMIT = MAX_CORE_BYTES + 128 * 1024

export type PublishOutcome =
  | { ok: true; manifest: Manifest; item: NewsItem }
  | { ok: false; status: number; error: PublishError | 'releases_disabled' }

// Publishes the Core DLL and posts its Change Log in one step; the Change Log
// is posted only once the release is in place. Shared with the admin port.
export async function publishRelease(ctx: AppContext, metadata: ReleaseMetadata, dll: Buffer): Promise<PublishOutcome> {
  if (!ctx.releases) return { ok: false, status: 503, error: 'releases_disabled' }
  const now = ctx.now()
  const result = await ctx.releases.publish(metadata.version, dll, changelogLines(metadata.body), now)
  if (!result.ok) return { ok: false, status: result.error === 'version_conflict' ? 409 : 400, error: result.error }
  const item = createNews(ctx.db, { kind: 'changelog', title: metadata.title, body: metadata.body, version: metadata.version }, now)
  ctx.hub.broadcast({ t: 'news.new', item })
  log.info(`release ${metadata.version} published (${dll.length} bytes)`)
  return { ok: true, manifest: result.manifest, item }
}

export function registerReleaseParser(app: FastifyInstance): void {
  app.addContentTypeParser(RELEASE_CONTENT_TYPE, { parseAs: 'buffer', bodyLimit: RELEASE_BODY_LIMIT }, (_request, body, done) =>
    done(null, body),
  )
}

function sendFile(reply: FastifyReply, bytes: Buffer | null, type: string): FastifyReply {
  if (bytes === null) return sendError(reply, 404, 'bad_request')
  // Loaders verify the signature themselves; caches must not mix versions.
  return reply.header('Cache-Control', 'no-store').type(type).send(bytes)
}

// Static-style files for MediconLoader (no sign-in: the loader runs before the
// client exists), and the owners' publish endpoint.
export function registerReleaseRoutes(app: FastifyInstance, ctx: AppContext): void {
  registerReleaseParser(app)
  const store = () => ctx.releases

  app.get('/medicon/manifest.json', async (_request, reply) =>
    sendFile(reply, store()?.file('manifest.json') ?? null, 'application/json'),
  )
  app.get('/medicon/manifest.sig', async (_request, reply) =>
    sendFile(reply, store()?.file('manifest.sig') ?? null, 'application/octet-stream'),
  )
  app.get<{ Params: { version: string; file: string } }>('/medicon/releases/:version/:file', async (request, reply) => {
    const { version, file } = request.params
    // The version names a directory: only the strict pattern reaches the disk.
    if (!VERSION_PATTERN.test(version)) return sendError(reply, 404, 'bad_request')
    if (file === CORE_FILE) return sendFile(reply, store()?.file(CORE_FILE, version) ?? null, 'application/octet-stream')
    if (file === 'manifest.json') return sendFile(reply, store()?.file('manifest.json', version) ?? null, 'application/json')
    if (file === 'manifest.sig') return sendFile(reply, store()?.file('manifest.sig', version) ?? null, 'application/octet-stream')
    return sendError(reply, 404, 'bad_request')
  })

  // Owners only, like news.post: the Admin tab's Change Log with a DLL attached.
  // Check before parsing the large body, so a disabled server or expired session
  // does not make the owner wait for the entire DLL to upload before failing.
  const authenticate = requireUser(ctx)
  const requirePublisher = async (request: FastifyRequest, reply: FastifyReply) => {
    const rejected = await authenticate(request, reply)
    if (rejected) return rejected
    if (request.user?.rank !== 'owner') return sendError(reply, 403, 'forbidden')
    if (!ctx.releases) return sendError(reply, 503, 'releases_disabled')
    return undefined
  }
  app.get('/releases/status', { onRequest: requirePublisher }, async () => ({
    latest_version: ctx.releases!.latestVersion(),
    max_core_bytes: MAX_CORE_BYTES,
  }))
  // Recheck after reception too: a session may expire or be revoked mid-upload.
  app.post('/releases', { onRequest: requirePublisher, preHandler: requirePublisher, bodyLimit: RELEASE_BODY_LIMIT }, async (request, reply) => {
    if (!Buffer.isBuffer(request.body)) return sendError(reply, 400, 'bad_release')
    const upload = parseReleaseUpload(request.body)
    if (!upload) return sendError(reply, 400, 'bad_release')
    const outcome = await publishRelease(ctx, upload.metadata, upload.dll)
    if (!outcome.ok) return sendError(reply, outcome.status, outcome.error)
    return { manifest: outcome.manifest, item: outcome.item }
  })
}
