import { createHash, createPrivateKey, sign, constants, type KeyObject } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { z } from 'zod'

// Core DLL releases for MediconLoader (docs/auto-update.md). The filesystem is
// the source of truth: releases/<version>/{MediconCore.dll, manifest.json,
// manifest.sig} and releases/latest naming the current version.
//
// The signing key never lives in the repository. RELEASE_SIGNING_KEY names a
// PEM file on the server; the loader embeds only the public half.

export const CORE_FILE = 'MediconCore.dll'
export const LOADER_API = 1
export const MAX_CORE_BYTES = 64 * 1024 * 1024
const MIN_CORE_BYTES = 1024
const MAX_CHANGELOG_LINES = 50

// major.minor.patch only: the version also names a directory.
export const VERSION_PATTERN = /^(0|[1-9]\d{0,3})\.(0|[1-9]\d{0,3})\.(0|[1-9]\d{0,3})$/

export function compareVersions(a: string, b: string): number {
  const left = a.split('.')
  const right = b.split('.')
  for (let i = 0; i < 2; i++) {
    const difference = Number(left[i] ?? 0) - Number(right[i] ?? 0)
    if (difference !== 0) return Math.sign(difference)
  }
  // Final digits are nested revisions, matching MediconLoader:
  // 1.0.2 < 1.0.25 < 1.0.28 < 1.0.3 (28 means revision 8 inside 2).
  const leftRevision = left[2] ?? '0'
  const rightRevision = right[2] ?? '0'
  return leftRevision === rightRevision ? 0 : leftRevision < rightRevision ? -1 : 1
}

export interface Manifest {
  version: string
  channel: 'stable'
  mcbe_version: string
  loader_api: number
  download_url: string
  sha256: string
  size: number
  changelog: string[]
  published_at: number
}

export interface ReleaseOptions {
  dir: string
  // https://host[:port] the loader downloads from; must be on its allowlist.
  publicBaseUrl: string
  signingKey: KeyObject
  mcbeVersion: string
}

export type PublishError = 'bad_release' | 'version_conflict'
export type PublishResult = { ok: true; manifest: Manifest } | { ok: false; error: PublishError }

// RSA-PSS / SHA-256, salt 32 bytes, over the exact manifest bytes served.
export function signManifest(key: KeyObject, bytes: Buffer): Buffer {
  return sign('sha256', bytes, { key, padding: constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 })
}

export function loadSigningKey(path: string): KeyObject {
  const key = createPrivateKey(readFileSync(path))
  const bits = key.asymmetricKeyDetails?.modulusLength ?? 0
  if (key.asymmetricKeyType !== 'rsa' || bits < 3072) {
    throw new Error('RELEASE_SIGNING_KEY は 3072 bit 以上の RSA 秘密鍵にしてください')
  }
  return key
}

// A plain sanity check, not trust: x64 PE image marked as a DLL. Trust comes
// from the owner's session and the signature over the manifest.
export function isX64Dll(bytes: Buffer): boolean {
  if (bytes.length < MIN_CORE_BYTES || bytes.length > MAX_CORE_BYTES) return false
  if (bytes[0] !== 0x4d || bytes[1] !== 0x5a) return false
  const pe = bytes.readUInt32LE(0x3c)
  if (pe > bytes.length - 24) return false
  if (bytes.readUInt32LE(pe) !== 0x00004550) return false
  const machine = bytes.readUInt16LE(pe + 4)
  const characteristics = bytes.readUInt16LE(pe + 22)
  return machine === 0x8664 && (characteristics & 0x2000) !== 0
}

export function changelogLines(body: string): string[] {
  return body
    .replace(/\r\n?/g, '\n')
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line !== '')
    .slice(0, MAX_CHANGELOG_LINES)
}

export class ReleaseStore {
  readonly options: ReleaseOptions
  private queue: Promise<unknown> = Promise.resolve()

  constructor(options: ReleaseOptions) {
    this.options = options
    mkdirSync(options.dir, { recursive: true })
  }

  latestVersion(): string | null {
    const path = join(this.options.dir, 'latest')
    if (!existsSync(path)) return null
    const version = readFileSync(path, 'utf8').trim()
    return VERSION_PATTERN.test(version) ? version : null
  }

  // The served files of one version, or of the latest when omitted.
  file(name: 'manifest.json' | 'manifest.sig' | typeof CORE_FILE, version?: string): Buffer | null {
    const chosen = version ?? this.latestVersion()
    if (chosen === null || !VERSION_PATTERN.test(chosen)) return null
    const path = join(this.options.dir, chosen, name)
    return existsSync(path) ? readFileSync(path) : null
  }

  // Publishes are serialized so two owners cannot race on `latest`.
  publish(version: string, dll: Buffer, changelog: string[], now: number): Promise<PublishResult> {
    const run = this.queue.then(() => this.publishNow(version, dll, changelog, now))
    this.queue = run.catch(() => undefined)
    return run
  }

  private publishNow(version: string, dll: Buffer, changelog: string[], now: number): PublishResult {
    if (!VERSION_PATTERN.test(version) || !isX64Dll(dll)) return { ok: false, error: 'bad_release' }
    const latest = this.latestVersion()
    if (latest !== null && compareVersions(version, latest) <= 0) return { ok: false, error: 'version_conflict' }
    const target = join(this.options.dir, version)
    if (existsSync(target)) return { ok: false, error: 'version_conflict' }

    const manifest: Manifest = {
      version,
      channel: 'stable',
      mcbe_version: this.options.mcbeVersion,
      loader_api: LOADER_API,
      download_url: `${this.options.publicBaseUrl}/medicon/releases/${version}/${CORE_FILE}`,
      sha256: createHash('sha256').update(dll).digest('hex'),
      size: dll.length,
      changelog,
      published_at: now,
    }
    const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + '\n', 'utf8')
    const signature = signManifest(this.options.signingKey, manifestBytes)

    // Build the whole version in a staging directory, then rename it into
    // place: a half-written release is never visible under its version.
    const staging = join(this.options.dir, `.staging-${version}-${process.pid}`)
    rmSync(staging, { recursive: true, force: true })
    try {
      mkdirSync(staging)
      writeFileSync(join(staging, CORE_FILE), dll)
      writeFileSync(join(staging, 'manifest.json'), manifestBytes)
      writeFileSync(join(staging, 'manifest.sig'), signature)
      renameSync(staging, target)
    } catch (err) {
      rmSync(staging, { recursive: true, force: true })
      throw err
    }
    const pointer = join(this.options.dir, 'latest.tmp')
    writeFileSync(pointer, version + '\n')
    renameSync(pointer, join(this.options.dir, 'latest'))
    return { ok: true, manifest }
  }
}

// The upload body: "MDCR", u32le metadata length, UTF-8 JSON metadata, DLL bytes.
// One request carries the Change Log post and the DLL, so neither is published alone.
export const RELEASE_CONTENT_TYPE = 'application/vnd.medicon.release'
const MAGIC = 0x5243444d // "MDCR"

export const releaseMetadata = z.object({
  version: z.string().regex(VERSION_PATTERN),
  title: z.string().trim().min(1).max(200),
  body: z
    .string()
    .transform((body) => body.replace(/\r\n?/g, '\n').trim())
    .pipe(z.string().min(1).max(20_000)),
})
export type ReleaseMetadata = z.infer<typeof releaseMetadata>

export function parseReleaseUpload(body: Buffer): { metadata: ReleaseMetadata; dll: Buffer } | null {
  if (body.length < 8 || body.readUInt32LE(0) !== MAGIC) return null
  const length = body.readUInt32LE(4)
  if (length === 0 || length > 64 * 1024 || 8 + length > body.length) return null
  let json: unknown
  try {
    json = JSON.parse(body.subarray(8, 8 + length).toString('utf8'))
  } catch {
    return null
  }
  const metadata = releaseMetadata.safeParse(json)
  if (!metadata.success) return null
  return { metadata: metadata.data, dll: body.subarray(8 + length) }
}

export function buildReleaseUpload(metadata: ReleaseMetadata, dll: Buffer): Buffer {
  const json = Buffer.from(JSON.stringify(metadata), 'utf8')
  const header = Buffer.alloc(8)
  header.writeUInt32LE(MAGIC, 0)
  header.writeUInt32LE(json.length, 4)
  return Buffer.concat([header, json, dll])
}
