import type { Db } from '../db/db.ts'

export interface UserRef {
  id: number
  name: string
}

// Owners may post News and Change Log from the client.
export type Rank = 'user' | 'owner'
export const RANKS: readonly Rank[] = ['user', 'owner']

// A signed-in user: who they are and what they may do.
export interface Member extends UserRef {
  rank: Rank
}

export interface User extends UserRef {
  banned: boolean
  rank: Rank
}

// Spec 2.1: 3-16 ASCII letters, digits or underscores. Uniqueness ignores case (COLLATE NOCASE).
export const USERNAME_PATTERN = /^[A-Za-z0-9_]{3,16}$/

interface UserRow {
  id: number
  name: string
  banned: number
  rank: Rank
}

export function findUserByName(db: Db, name: string): User | undefined {
  const row = db.prepare<[string], UserRow>('SELECT id, name, banned, rank FROM users WHERE name = ?').get(name)
  return row && { id: row.id, name: row.name, banned: row.banned === 1, rank: row.rank }
}

export function createUser(db: Db, name: string, now: number): User {
  const result = db.prepare('INSERT INTO users (name, created_at) VALUES (?, ?)').run(name, now)
  return { id: Number(result.lastInsertRowid), name, banned: false, rank: 'user' }
}

export function setRank(db: Db, userId: number, rank: Rank): void {
  db.prepare('UPDATE users SET rank = ? WHERE id = ?').run(rank, userId)
}

export const toMember = (user: { id: number; name: string; rank: Rank }): Member => ({
  id: user.id,
  name: user.name,
  rank: user.rank,
})

export function setBanned(db: Db, userId: number, banned: boolean): void {
  db.prepare('UPDATE users SET banned = ? WHERE id = ?').run(banned ? 1 : 0, userId)
}
