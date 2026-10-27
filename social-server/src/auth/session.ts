import { randomBytes } from 'node:crypto'
import type { Db } from '../db/db.ts'
import type { Member, Rank } from '../services/users.ts'
import { sha256Hex } from './hash.ts'

export const SESSION_TTL_MS = 24 * 60 * 60 * 1000

export interface Session {
  user: Member
  expiresAt: number
}

// Only the token's hash is stored, like license keys.
export function issueSession(db: Db, userId: number, now: number): { token: string; expiresAt: number } {
  const token = randomBytes(32).toString('base64url')
  const expiresAt = now + SESSION_TTL_MS
  db.prepare('INSERT INTO sessions (token_hash, user_id, expires_at) VALUES (?, ?, ?)').run(
    sha256Hex(token),
    userId,
    expiresAt,
  )
  return { token, expiresAt }
}

interface SessionRow {
  id: number
  name: string
  banned: number
  rank: Rank
  expiresAt: number
}

export function verifyToken(db: Db, token: string, now: number): Session | null {
  const row = db
    .prepare<[string], SessionRow>(
      `SELECT u.id, u.name, u.banned, u.rank, s.expires_at AS expiresAt
       FROM sessions s JOIN users u ON u.id = s.user_id
       WHERE s.token_hash = ?`,
    )
    .get(sha256Hex(token))
  if (!row || row.expiresAt <= now || row.banned === 1) return null
  return { user: { id: row.id, name: row.name, rank: row.rank }, expiresAt: row.expiresAt }
}

export function deleteUserSessions(db: Db, userId: number): number {
  return db.prepare('DELETE FROM sessions WHERE user_id = ?').run(userId).changes
}

export function deleteExpiredSessions(db: Db, now: number): number {
  return db.prepare('DELETE FROM sessions WHERE expires_at <= ?').run(now).changes
}

export function bearerToken(header: string | undefined): string | null {
  const match = header === undefined ? null : /^Bearer\s+(\S+)\s*$/i.exec(header)
  return match?.[1] ?? null
}
