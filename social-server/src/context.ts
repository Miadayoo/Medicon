import type { Db } from './db/db.ts'
import { LIMITS, RateLimiter } from './ratelimit.ts'
import { createHandlers } from './ws/handlers.ts'
import { Hub } from './ws/hub.ts'

// Spec 2.4: clients ping every 30 s, and Cloudflare drops idle WebSockets after about 100 s.
export const IDLE_TIMEOUT_MS = 75_000

// Shared by the public server, the admin server and the WebSocket hub.
export interface AppContext {
  db: Db
  hub: Hub
  now: () => number
  limits: { auth: RateLimiter; global: RateLimiter; dm: RateLimiter; friend: RateLimiter }
}

export interface ContextOptions {
  now?: () => number
  idleTimeoutMs?: number
}

export function createContext(db: Db, options: ContextOptions = {}): AppContext {
  const now = options.now ?? Date.now
  const ctx: AppContext = {
    db,
    now,
    hub: new Hub({ now, idleTimeoutMs: options.idleTimeoutMs ?? IDLE_TIMEOUT_MS }),
    limits: {
      auth: new RateLimiter(LIMITS.auth),
      global: new RateLimiter(LIMITS.global),
      dm: new RateLimiter(LIMITS.dm),
      friend: new RateLimiter(LIMITS.friend),
    },
  }
  ctx.hub.setEvents(createHandlers(ctx))
  return ctx
}
