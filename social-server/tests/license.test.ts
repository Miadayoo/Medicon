import { describe, expect, it } from 'vitest'
import { generateKey, hashKey, normalizeKey } from '../src/auth/license.ts'

describe('license keys', () => {
  it('use the MDC-XXXX-XXXX-XXXX-XXXX Crockford Base32 format', () => {
    for (let i = 0; i < 500; i++) expect(generateKey()).toMatch(/^MDC(-[0-9A-HJKMNP-TV-Z]{4}){4}$/)
  })

  it('spread randomness over every character', () => {
    const keys = Array.from({ length: 2000 }, () => generateKey().replaceAll('-', '').slice(3))
    expect(new Set(keys).size).toBe(keys.length)
    // A broken bit extraction would leave some positions stuck on a few characters.
    for (let position = 0; position < 16; position++) {
      expect(new Set(keys.map((key) => key[position])).size).toBe(32)
    }
  })

  it('normalize case and whitespace before hashing', () => {
    expect(normalizeKey(' mdc-abcd-efgh-jkmn-pqrs\n')).toBe('MDC-ABCD-EFGH-JKMN-PQRS')
    expect(normalizeKey('MDC - ABCD-EFGH-JKMN-PQRS')).toBe('MDC-ABCD-EFGH-JKMN-PQRS')
    expect(hashKey(' mdc-abcd-efgh-jkmn-pqrs ')).toBe(hashKey('MDC-ABCD-EFGH-JKMN-PQRS'))
    expect(hashKey('MDC-ABCD-EFGH-JKMN-PQRS')).toMatch(/^[0-9a-f]{64}$/)
  })
})
