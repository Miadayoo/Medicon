import { createAdminApp } from './admin/server.ts'
import { createApp } from './app.ts'
import { deleteExpiredSessions } from './auth/session.ts'
import { loadConfig, loadEnvFile, type Config } from './config.ts'
import { createContext } from './context.ts'
import { startBackups } from './db/backup.ts'
import { openDb } from './db/db.ts'
import { log } from './log.ts'
import { loadSigningKey, ReleaseStore } from './services/releases.ts'
import { CLOSE } from './ws/protocol.ts'

// Never 0.0.0.0: the outside world reaches this server only through Cloudflare Tunnel.
const HOST = '127.0.0.1'
const HOUR_MS = 60 * 60 * 1000

loadEnvFile()
let config: Config
try {
  config = loadConfig()
} catch (err) {
  log.error(err instanceof Error ? err.message : String(err))
  process.exit(1)
}

let releases: ReleaseStore | null = null
if (config.releaseSigningKey !== undefined && config.publicBaseUrl !== undefined) {
  try {
    releases = new ReleaseStore({
      dir: config.releasesDir,
      publicBaseUrl: config.publicBaseUrl,
      signingKey: loadSigningKey(config.releaseSigningKey),
      mcbeVersion: config.mcbeVersion,
    })
    log.info(`Core releases enabled (latest ${releases.latestVersion() ?? 'none'})`)
  } catch (err) {
    log.error('Core releases disabled: the signing key could not be loaded', err instanceof Error ? err.message : err)
  }
} else {
  log.info('Core releases disabled (set RELEASE_SIGNING_KEY and PUBLIC_BASE_URL to publish)')
}

const db = openDb(config.dbPath)
const ctx = createContext(db, { releases })
const app = createApp(ctx)
const admin = createAdminApp(ctx, config.adminSecret)

const sweep = (): void => {
  const now = Date.now()
  deleteExpiredSessions(db, now)
  for (const limiter of Object.values(ctx.limits)) limiter.prune(now)
}
sweep()
const sweepTimer = setInterval(sweep, HOUR_MS)
const backups = startBackups(db, config.backupDir)

try {
  await app.listen({ host: HOST, port: config.port })
  await admin.listen({ host: HOST, port: config.adminPort })
} catch (err) {
  log.error('failed to start', err)
  process.exit(1)
}
log.info(`listening on http://${HOST}:${config.port} (admin http://${HOST}:${config.adminPort})`)

let stopping = false

async function shutdown(signal: string): Promise<void> {
  if (stopping) return
  stopping = true
  log.info(`${signal} received, shutting down`)
  clearInterval(sweepTimer)
  backups.stop()
  // Stop accepting first; the HTTP servers finish closing once the upgraded sockets are gone.
  const closing = Promise.allSettled([app.close(), admin.close()])
  await ctx.hub.closeAll(CLOSE.goingAway, 'server_shutdown')
  await closing
  db.close()
  log.info('stopped')
  process.exit(0)
}

process.on('SIGINT', () => void shutdown('SIGINT'))
process.on('SIGTERM', () => void shutdown('SIGTERM'))
