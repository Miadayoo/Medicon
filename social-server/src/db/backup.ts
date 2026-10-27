import { mkdirSync, readdirSync, rmSync, statSync } from 'node:fs'
import { join } from 'node:path'
import type { Db } from './db.ts'
import { log } from '../log.ts'

const DAY_MS = 24 * 60 * 60 * 1000
// One backup a day, so this keeps about two weeks.
const KEEP = 14
const FILE_PATTERN = /^social-\d{8}-\d{6}\.db$/

export interface BackupSchedule {
  stop(): void
}

export function startBackups(db: Db, dir: string): BackupSchedule {
  const run = (): void => {
    backupNow(db, dir).catch((err: unknown) => log.error('backup failed', err))
  }
  // A server restarted every few hours would otherwise never reach its first daily run.
  if (Date.now() - latestBackupTime(dir) >= DAY_MS) run()
  const timer = setInterval(run, DAY_MS)
  return { stop: () => clearInterval(timer) }
}

export async function backupNow(db: Db, dir: string, now = new Date()): Promise<string> {
  mkdirSync(dir, { recursive: true })
  const file = join(dir, `social-${timestamp(now)}.db`)
  await db.backup(file)
  pruneBackups(dir)
  return file
}

function backupFiles(dir: string): string[] {
  try {
    return readdirSync(dir).filter((name) => FILE_PATTERN.test(name)).sort()
  } catch {
    return []
  }
}

function latestBackupTime(dir: string): number {
  const latest = backupFiles(dir).at(-1)
  return latest === undefined ? 0 : statSync(join(dir, latest)).mtimeMs
}

function pruneBackups(dir: string): void {
  const files = backupFiles(dir)
  for (const name of files.slice(0, Math.max(0, files.length - KEEP))) rmSync(join(dir, name))
}

function timestamp(date: Date): string {
  const pad = (value: number): string => String(value).padStart(2, '0')
  return (
    `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}-` +
    `${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`
  )
}
