import { existsSync } from 'node:fs'
import { isAbsolute, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { z } from 'zod'

// Relative paths resolve against social-server/, not the working directory,
// so a service manager started elsewhere still finds the same files.
export const ROOT_DIR = fileURLToPath(new URL('..', import.meta.url))

const port = z.coerce.number().int().min(1).max(65535)

const envSchema = z.object({
  PORT: port.default(8787),
  ADMIN_PORT: port.default(8788),
  DB_PATH: z.string().min(1).default('./data/social.db'),
  ADMIN_SECRET: z.string().min(16, 'ADMIN_SECRET は16文字以上にしてください'),
  BACKUP_DIR: z.string().min(1).default('./backups'),
})

export interface Config {
  port: number
  adminPort: number
  dbPath: string
  adminSecret: string
  backupDir: string
}

// Values already set in the environment win over .env (process.loadEnvFile never overrides).
export function loadEnvFile(): void {
  const path = resolve(ROOT_DIR, '.env')
  if (existsSync(path)) process.loadEnvFile(path)
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const parsed = envSchema.safeParse(env)
  if (!parsed.success) {
    const detail = parsed.error.issues.map((issue) => `${issue.path.join('.')}: ${issue.message}`).join(', ')
    throw new Error(`.env の設定が不正です (${detail})`)
  }
  const values = parsed.data
  return {
    port: values.PORT,
    adminPort: values.ADMIN_PORT,
    dbPath: resolvePath(values.DB_PATH),
    adminSecret: values.ADMIN_SECRET,
    backupDir: resolvePath(values.BACKUP_DIR),
  }
}

function resolvePath(path: string): string {
  return isAbsolute(path) ? path : resolve(ROOT_DIR, path)
}
