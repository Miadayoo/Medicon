import { z } from 'zod'
import type { Db } from '../db/db.ts'

export type NewsKind = 'news' | 'changelog'

export interface NewsItem {
  id: number
  kind: NewsKind
  title: string
  body: string
  version: string | null
  createdAt: number
}

export interface NewsQuery {
  kind: NewsKind
  limit: number
  before?: number | undefined
}

// Newest first. `before` is an exclusive id cursor for loading older items.
export function listNews(db: Db, query: NewsQuery): NewsItem[] {
  return db
    .prepare<[NewsKind, number, number], NewsItem>(
      `SELECT id, kind, title, body, version, created_at AS createdAt
       FROM news WHERE kind = ? AND id < ?
       ORDER BY id DESC LIMIT ?`,
    )
    .all(query.kind, query.before ?? Number.MAX_SAFE_INTEGER, query.limit)
}

// What the admin CLI and owners post: trimmed, CRLF normalized, bounded.
export const newsInput = z.object({
  kind: z.enum(['news', 'changelog']),
  title: z.string().trim().min(1).max(200),
  body: z
    .string()
    .transform((body) => body.replace(/\r\n?/g, '\n').trim())
    .pipe(z.string().min(1).max(20_000)),
  version: z
    .string()
    .trim()
    .max(32)
    .optional()
    .transform((version) => (version === '' ? undefined : version)),
})

export interface NewNews {
  kind: NewsKind
  title: string
  body: string
  version?: string | undefined
}

export function createNews(db: Db, input: NewNews, now: number): NewsItem {
  const version = input.version ?? null
  const result = db
    .prepare('INSERT INTO news (kind, title, body, version, created_at) VALUES (?, ?, ?, ?, ?)')
    .run(input.kind, input.title, input.body, version, now)
  return {
    id: Number(result.lastInsertRowid),
    kind: input.kind,
    title: input.title,
    body: input.body,
    version,
    createdAt: now,
  }
}
