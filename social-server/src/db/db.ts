import { mkdirSync, readFileSync } from 'node:fs'
import { dirname } from 'node:path'
import Database from 'better-sqlite3'

export type Db = Database.Database

const SCHEMA = readFileSync(new URL('./schema.sql', import.meta.url), 'utf8')

// ':memory:' opens a throwaway database for tests.
export function openDb(path: string): Db {
  if (path !== ':memory:') mkdirSync(dirname(path), { recursive: true })
  const db = new Database(path)
  db.pragma('journal_mode = WAL')
  db.pragma('synchronous = NORMAL')
  db.pragma('foreign_keys = ON')
  db.pragma('busy_timeout = 5000')
  db.exec(SCHEMA)
  migrate(db)
  return db
}

// Columns added after a database may already exist. CREATE TABLE IF NOT EXISTS
// never changes an existing table, so they are added here once.
function migrate(db: Db): void {
  const columns = db.prepare<[], { name: string }>("SELECT name FROM pragma_table_info('users')").all()
  if (!columns.some((column) => column.name === 'rank')) {
    db.exec("ALTER TABLE users ADD COLUMN rank TEXT NOT NULL DEFAULT 'user' CHECK (rank IN ('user', 'owner'))")
  }
}
