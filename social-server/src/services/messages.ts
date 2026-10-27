import type { Db } from '../db/db.ts'
import type { ErrorCode } from '../errors.ts'
import type { Rank, UserRef } from './users.ts'

// Spec 2.4.
// The author's rank lets clients mark owners next to their name.
export interface Author extends UserRef {
  rank: Rank
}

export interface Message {
  id: number
  author: Author
  body: string
  createdAt: number
}

export interface DmMessage extends Message {
  to: number
}

export interface PageQuery {
  limit: number
  before?: number | undefined
}

// Spec 2.1 / 2.5: 1-300 Unicode code points once surrounding whitespace is removed.
export const MAX_BODY_CODE_POINTS = 300

export type BodyResult = { ok: true; body: string } | { ok: false; error: Extract<ErrorCode, 'empty' | 'too_long'> }

export function checkBody(raw: string): BodyResult {
  const body = raw.trim()
  if (body.length === 0) return { ok: false, error: 'empty' }
  let count = 0
  for (const _ of body) if (++count > MAX_BODY_CODE_POINTS) return { ok: false, error: 'too_long' }
  return { ok: true, body }
}

interface MessageRow {
  id: number
  authorId: number
  authorName: string
  authorRank: Rank
  body: string
  createdAt: number
}

interface DmRow extends MessageRow {
  to: number
}

const toMessage = (row: MessageRow): Message => ({
  id: row.id,
  author: { id: row.authorId, name: row.authorName, rank: row.authorRank },
  body: row.body,
  createdAt: row.createdAt,
})

const toDm = (row: DmRow): DmMessage => ({ ...toMessage(row), to: row.to })

export function createGlobalMessage(db: Db, author: Author, body: string, now: number): Message {
  const result = db.prepare('INSERT INTO global_messages (author_id, body, created_at) VALUES (?, ?, ?)').run(author.id, body, now)
  return { id: Number(result.lastInsertRowid), author, body, createdAt: now }
}

// Newest first; deleted messages are never returned (spec 3.3).
export function listGlobalMessages(db: Db, query: PageQuery): Message[] {
  return db
    .prepare<[number, number], MessageRow>(
      `SELECT m.id, m.author_id AS authorId, u.name AS authorName, u.rank AS authorRank, m.body, m.created_at AS createdAt
       FROM global_messages m JOIN users u ON u.id = m.author_id
       WHERE m.deleted = 0 AND m.id < ?
       ORDER BY m.id DESC LIMIT ?`,
    )
    .all(query.before ?? Number.MAX_SAFE_INTEGER, query.limit)
    .map(toMessage)
}

export function createDm(db: Db, author: Author, to: number, body: string, now: number): DmMessage {
  const result = db
    .prepare('INSERT INTO dm_messages (sender_id, recipient_id, body, created_at) VALUES (?, ?, ?, ?)')
    .run(author.id, to, body, now)
  return { id: Number(result.lastInsertRowid), author, to, body, createdAt: now }
}

// Both directions of the conversation between `me` and `other`, newest first.
export function listDms(db: Db, me: number, other: number, query: PageQuery): DmMessage[] {
  return db
    .prepare<[number, number, number, number, number, number], DmRow>(
      `SELECT m.id, m.sender_id AS authorId, u.name AS authorName, u.rank AS authorRank, m.recipient_id AS "to", m.body,
              m.created_at AS createdAt
       FROM dm_messages m JOIN users u ON u.id = m.sender_id
       WHERE m.deleted = 0 AND m.id < ?
         AND ((m.sender_id = ? AND m.recipient_id = ?) OR (m.sender_id = ? AND m.recipient_id = ?))
       ORDER BY m.id DESC LIMIT ?`,
    )
    .all(query.before ?? Number.MAX_SAFE_INTEGER, me, other, other, me, query.limit)
    .map(toDm)
}

// Moves the read mark forward only; an older upTo is ignored. Returns the mark.
export function markRead(db: Db, reader: number, other: number, upTo: number): number {
  db.prepare(
    `INSERT INTO dm_reads (reader_id, other_id, up_to) VALUES (?, ?, ?)
     ON CONFLICT (reader_id, other_id) DO UPDATE SET up_to = max(up_to, excluded.up_to)`,
  ).run(reader, other, upTo)
  return readUpTo(db, reader, other)
}

// How far `reader` has read the messages `other` sent them.
export function readUpTo(db: Db, reader: number, other: number): number {
  const row = db
    .prepare<[number, number], { upTo: number }>('SELECT up_to AS upTo FROM dm_reads WHERE reader_id = ? AND other_id = ?')
    .get(reader, other)
  return row?.upTo ?? 0
}

// Spec 3.3: DMs from `other` to `me` past my read mark, not deleted.
export function unreadCount(db: Db, me: number, other: number): number {
  const row = db
    .prepare<[number, number, number], { count: number }>(
      `SELECT count(*) AS count FROM dm_messages
       WHERE sender_id = ? AND recipient_id = ? AND deleted = 0 AND id > ?`,
    )
    .get(other, me, readUpTo(db, me, other))
  return row?.count ?? 0
}
