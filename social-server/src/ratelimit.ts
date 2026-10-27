// Spec 2.5.
export const LIMITS = {
  auth: { count: 10, windowMs: 10 * 60 * 1000 },
  // Spec 2.5 says 2 s; shortened to 0.3 s at the owner's request (2026-10-27).
  global: { count: 1, windowMs: 300 },
  dm: { count: 5, windowMs: 5 * 1000 },
  friend: { count: 10, windowMs: 60 * 60 * 1000 },
} as const

// Sliding window: allows `count` hits per key within any `windowMs` span.
export class RateLimiter {
  private readonly hits = new Map<string, number[]>()
  private readonly count: number
  private readonly windowMs: number

  constructor(limit: { count: number; windowMs: number }) {
    this.count = limit.count
    this.windowMs = limit.windowMs
  }

  // Records the hit and returns true, or returns false when the key is over its limit.
  tryHit(key: string, now: number): boolean {
    const since = now - this.windowMs
    const recent = (this.hits.get(key) ?? []).filter((time) => time > since)
    const allowed = recent.length < this.count
    if (allowed) recent.push(now)
    this.hits.set(key, recent)
    return allowed
  }

  // Forgets keys with no hit inside the window, so one-off keys don't pile up.
  prune(now: number): void {
    const since = now - this.windowMs
    for (const [key, times] of this.hits) {
      const latest = times.at(-1)
      if (latest === undefined || latest <= since) this.hits.delete(key)
    }
  }
}
