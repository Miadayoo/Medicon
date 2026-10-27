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
  // Core DLL releases for MediconLoader. Publishing needs both a signing key
  // (a PEM file outside the repository) and the public https base URL.
  RELEASES_DIR: z.string().min(1).default('./releases'),
  RELEASE_SIGNING_KEY: z.string().optional().transform((value) => (value?.trim() ? value.trim() : undefined)),
  PUBLIC_BASE_URL: z
    .string()
    .optional()
    .transform((value) => (value?.trim() ? value.trim().replace(/\/+$/, '') : undefined))
    .refine((value) => value === undefined || /^https:\/\/[a-z0-9.-]+(:\d{1,5})?$/i.test(value), {
      message: 'PUBLIC_BASE_URL は https://host の形にしてください（パスなし）',
    }),
  RELEASE_MCBE_VERSION: z.string().regex(/^\d+\.\d+\.\d+$/).default('1.26.52'),
})

export interface Config {
  port: number
  adminPort: number
  dbPath: string
  adminSecret: string
  backupDir: string
  releasesDir: string
  releaseSigningKey: string | undefined
  publicBaseUrl: string | undefined
  mcbeVersion: string
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
    releasesDir: resolvePath(values.RELEASES_DIR),
    releaseSigningKey: values.RELEASE_SIGNING_KEY === undefined ? undefined : resolvePath(values.RELEASE_SIGNING_KEY),
    publicBaseUrl: values.PUBLIC_BASE_URL,
    mcbeVersion: values.RELEASE_MCBE_VERSION,
  }
}

function resolvePath(path: string): string {
  return isAbsolute(path) ? path : resolve(ROOT_DIR, path)
}
