import { mkdtempSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import Database from 'better-sqlite3'
import { afterEach, describe, expect, it } from 'vitest'
import { backupNow } from '../src/db/backup.ts'
import { openDb } from '../src/db/db.ts'

let dir: string

afterEach(() => {
  rmSync(dir, { recursive: true, force: true })
})

describe('backupNow', () => {
  it('writes a readable copy and keeps only the newest 14 backups', async () => {
    dir = mkdtempSync(join(tmpdir(), 'social-backup-'))
    for (let day = 1; day <= 20; day++) {
      writeFileSync(join(dir, `social-202601${String(day).padStart(2, '0')}-030000.db`), '')
    }
    writeFileSync(join(dir, 'notes.txt'), 'not a backup')
    const db = openDb(':memory:')
    db.prepare('INSERT INTO users (name, created_at) VALUES (?, ?)').run('faokun', 1)

    const file = await backupNow(db, dir, new Date(2026, 9, 27, 4, 5, 6))
    db.close()

    expect(file).toBe(join(dir, 'social-20261027-040506.db'))
    const files = readdirSync(dir)
    expect(files.filter((name) => name.endsWith('.db'))).toHaveLength(14)
    expect(files).toContain('notes.txt')
    expect(files).not.toContain('social-20260107-030000.db')
    expect(files).toContain('social-20260108-030000.db')

    const copy = new Database(file, { readonly: true })
    expect(copy.prepare('SELECT name FROM users').get()).toEqual({ name: 'faokun' })
    copy.close()
  })
})
