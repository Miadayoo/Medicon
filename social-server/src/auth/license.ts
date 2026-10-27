import { randomBytes } from 'node:crypto'
import type { Db } from '../db/db.ts'
import type { Member, Rank, UserRef } from '../services/users.ts'
import { sha256Hex } from './hash.ts'

const CROCKFORD = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'

// 80 random bits as 16 Crockford Base32 characters: MDC-XXXX-XXXX-XXXX-XXXX.
export function generateKey(): string {
  let buffer = 0
  let bits = 0
  let chars = ''
  for (const byte of randomBytes(10)) {
    buffer = (buffer << 8) | byte
    bits += 8
    while (bits >= 5) {
      bits -= 5
      chars += CROCKFORD.charAt((buffer >> bits) & 31)
    }
    buffer &= (1 << bits) - 1
  }
  return `MDC-${chars.slice(0, 4)}-${chars.slice(4, 8)}-${chars.slice(8, 12)}-${chars.slice(12)}`
}

export function normalizeKey(key: string): string {
  return key.toUpperCase().replace(/\s+/g, '')
}

// Only this hash is stored. The plain key is shown once, when the CLI issues it.
export function hashKey(key: string): string {
  return sha256Hex(normalizeKey(key))
}

export function addLicense(db: Db, userId: number, now: number): string {
  const key = generateKey()
  db.prepare('INSERT INTO licenses (key_hash, user_id, created_at) VALUES (?, ?, ?)').run(hashKey(key), userId, now)
  return key
}

export function revokeLicenses(db: Db, userId: number): number {
  return db.prepare("UPDATE licenses SET status = 'revoked' WHERE user_id = ? AND status = 'active'").run(userId)
    .changes
}

export type KeyCheck = { ok: true; user: Member } | { ok: false; error: 'invalid_key' | 'revoked' | 'banned' }

interface KeyRow {
  licenseId: number
  status: string
  userId: number
  name: string
  banned: number
  rank: Rank
}

export function checkKey(db: Db, key: string, now: number): KeyCheck {
  const row = db
    .prepare<[string], KeyRow>(
      `SELECT l.id AS licenseId, l.status, u.id AS userId, u.name, u.banned, u.rank
       FROM licenses l JOIN users u ON u.id = l.user_id
       WHERE l.key_hash = ?`,
    )
    .get(hashKey(key))
  if (!row) return { ok: false, error: 'invalid_key' }
  // A ban outlives revoked keys; report it first so the user sees the real reason.
  if (row.banned === 1) return { ok: false, error: 'banned' }
  if (row.status !== 'active') return { ok: false, error: 'revoked' }
  db.prepare('UPDATE licenses SET last_used_at = ? WHERE id = ?').run(now, row.licenseId)
  return { ok: true, user: { id: row.userId, name: row.name, rank: row.rank } }
}

export interface LicenseListing {
  name: string
  banned: boolean
  licenseId: number
  status: 'active' | 'revoked'
  createdAt: number
  lastUsedAt: number | null
}

interface ListingRow {
  name: string
  banned: number
  licenseId: number
  status: 'active' | 'revoked'
  createdAt: number
  lastUsedAt: number | null
}

export function listLicenses(db: Db): LicenseListing[] {
  return db
    .prepare<[], ListingRow>(
      `SELECT u.name, u.banned, l.id AS licenseId, l.status, l.created_at AS createdAt, l.last_used_at AS lastUsedAt
       FROM licenses l JOIN users u ON u.id = l.user_id
       ORDER BY u.name COLLATE NOCASE, l.id`,
    )
    .all()
    .map((row) => ({ ...row, banned: row.banned === 1 }))
}
