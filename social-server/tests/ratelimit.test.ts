import { describe, expect, it } from 'vitest'
import { RateLimiter } from '../src/ratelimit.ts'

describe('RateLimiter', () => {
  it('allows `count` hits within any window per key', () => {
    const limiter = new RateLimiter({ count: 2, windowMs: 1000 })
    expect(limiter.tryHit('a', 0)).toBe(true)
    expect(limiter.tryHit('a', 100)).toBe(true)
    expect(limiter.tryHit('a', 200)).toBe(false)
    expect(limiter.tryHit('b', 200)).toBe(true)
    // The hit at 0 has left the window; the one at 100 has not.
    expect(limiter.tryHit('a', 1000)).toBe(true)
    expect(limiter.tryHit('a', 1050)).toBe(false)
  })

  it('does not count rejected hits', () => {
    const limiter = new RateLimiter({ count: 1, windowMs: 1000 })
    expect(limiter.tryHit('a', 0)).toBe(true)
    expect(limiter.tryHit('a', 900)).toBe(false)
    expect(limiter.tryHit('a', 1001)).toBe(true)
  })

  it('keeps counting through prune', () => {
    const limiter = new RateLimiter({ count: 1, windowMs: 1000 })
    expect(limiter.tryHit('a', 0)).toBe(true)
    limiter.prune(500)
    expect(limiter.tryHit('a', 600)).toBe(false)
    limiter.prune(5000)
    expect(limiter.tryHit('a', 5000)).toBe(true)
  })
})
