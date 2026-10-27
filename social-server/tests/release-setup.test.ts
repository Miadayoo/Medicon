import { generateKeyPairSync } from 'node:crypto'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { parseEnv } from 'node:util'
import { afterEach, beforeAll, describe, expect, it } from 'vitest'
import { loadSigningKey } from '../src/services/releases.ts'
import { publicKeyHeader, setupReleases } from '../tools/release-setup.ts'

let dir: string | undefined
let pem: string
beforeAll(() => {
  pem = generateKeyPairSync('rsa', { modulusLength: 3072 }).privateKey.export({ format: 'pem', type: 'pkcs8' }).toString()
})
afterEach(() => {
  if (dir) rmSync(dir, { recursive: true, force: true })
  dir = undefined
})

function fixture(env = 'ADMIN_SECRET=preserve-this-secret\nDB_PATH=./data/custom.db\nRELEASE_SIGNING_KEY=\nPUBLIC_BASE_URL=\n') {
  dir = mkdtempSync(join(tmpdir(), 'medicon-release-setup-'))
  const root = join(dir, 'social-server')
  mkdirSync(root)
  writeFileSync(join(root, '.env.example'), 'ADMIN_SECRET=\nRELEASE_SIGNING_KEY=\nPUBLIC_BASE_URL=\n')
  writeFileSync(join(root, '.env'), env)
  return root
}

describe('release setup', () => {
  it('enables an existing server without changing its secret or replacing its signing key', () => {
    const root = fixture()
    const result = setupReleases(root)
    const env = parseEnv(readFileSync(join(root, '.env'), 'utf8'))
    expect(env).toMatchObject({ ADMIN_SECRET: 'preserve-this-secret', DB_PATH: './data/custom.db',
      RELEASE_SIGNING_KEY: 'keys/release-private.pem', PUBLIC_BASE_URL: 'https://medicon.auau1000.com' })
    const original = readFileSync(result.keyPath, 'utf8')
    const header = readFileSync(result.headerPath, 'utf8')
    expect(header).toBe(publicKeyHeader(loadSigningKey(result.keyPath)))
    expect(existsSync(result.releasesDir)).toBe(true)
    setupReleases(root)
    expect(readFileSync(result.keyPath, 'utf8')).toBe(original)
    expect(readFileSync(result.headerPath, 'utf8')).toBe(header)
  })

  it('reuses a configured key and URL and exports the header to the C++ tree when present', () => {
    const root = fixture('ADMIN_SECRET=preserved-secret\nRELEASE_SIGNING_KEY=private.pem\nPUBLIC_BASE_URL=https://updates.example.com/\nRELEASES_DIR=data/releases\n')
    writeFileSync(join(root, 'private.pem'), pem)
    const header = join(root, '../include/medicon/update/ReleasePublicKey.hpp')
    mkdirSync(dirname(header), { recursive: true })
    const result = setupReleases(root)
    expect(result.headerPath).toBe(header)
    expect(result.publicBaseUrl).toBe('https://updates.example.com')
    expect(result.releasesDir).toBe(join(root, 'data/releases'))
    expect(readFileSync(result.keyPath, 'utf8')).toBe(pem)
    expect(readFileSync(header, 'utf8')).not.toContain('PRIVATE KEY')
    expect(setupReleases(root, 'https://new.example.com').publicBaseUrl).toBe('https://new.example.com')
  })

  it('creates an env file from the template on first use', () => {
    const root = fixture()
    rmSync(join(root, '.env'))
    // Existing keygen output should work even before .env names the key.
    mkdirSync(join(root, 'keys'))
    writeFileSync(join(root, 'keys/release-private.pem'), pem)
    setupReleases(root)
    const env = parseEnv(readFileSync(join(root, '.env'), 'utf8'))
    expect(env.ADMIN_SECRET?.length).toBeGreaterThanOrEqual(16)
    expect(env.RELEASE_SIGNING_KEY).toBe('keys/release-private.pem')
  })

  it('does not silently rotate a missing key from an existing installation', () => {
    const root = fixture('RELEASE_SIGNING_KEY=lost.pem\n')
    expect(() => setupReleases(root)).toThrow('元の release-private.pem')
    expect(existsSync(join(root, 'lost.pem'))).toBe(false)
  })

  it('refuses a new key when existing releases or a loader public key remain', () => {
    const root = fixture()
    mkdirSync(join(root, 'releases'))
    writeFileSync(join(root, 'releases/latest'), '1.0.0\n')
    expect(() => setupReleases(root)).toThrow('元の release-private.pem')
    rmSync(join(root, 'releases/latest'))
    const header = join(root, 'keys/ReleasePublicKey.hpp')
    mkdirSync(dirname(header))
    writeFileSync(header, '// existing public key')
    expect(() => setupReleases(root)).toThrow('元の release-private.pem')
  })

  it('rejects invalid keys and URLs without rewriting the env file', () => {
    const root = fixture('RELEASE_SIGNING_KEY=bad.pem\n')
    writeFileSync(join(root, 'bad.pem'), 'invalid key')
    const original = readFileSync(join(root, '.env'), 'utf8')
    expect(() => setupReleases(root)).toThrow()
    expect(() => setupReleases(root, 'http://example.com/path')).toThrow('--base-url')
    expect(readFileSync(join(root, '.env'), 'utf8')).toBe(original)
  })
})
