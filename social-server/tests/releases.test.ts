import { constants, createHash, generateKeyPairSync, verify, type KeyObject } from 'node:crypto'
import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, beforeAll, describe, expect, it } from 'vitest'
import {
  buildReleaseUpload,
  compareVersions,
  parseReleaseUpload,
  RELEASE_CONTENT_TYPE,
  ReleaseStore,
} from '../src/services/releases.ts'
import { ADMIN_SECRET, connect, http, startServer, type TestServer } from './helpers.ts'

let keys: { privateKey: KeyObject; publicKey: KeyObject }
beforeAll(() => {
  keys = generateKeyPairSync('rsa', { modulusLength: 3072 })
})

let server: TestServer | undefined
let dir: string | undefined
afterEach(async () => {
  await server?.close()
  server = undefined
  if (dir) rmSync(dir, { recursive: true, force: true })
  dir = undefined
})

// The smallest image isX64Dll accepts: MZ, PE\0\0, AMD64, IMAGE_FILE_DLL.
function fakeDll(fill = 0x90, size = 4096): Buffer {
  const bytes = Buffer.alloc(size, fill)
  bytes.write('MZ', 0, 'latin1')
  bytes.writeUInt32LE(0x80, 0x3c)
  bytes.writeUInt32LE(0x00004550, 0x80)
  bytes.writeUInt16LE(0x8664, 0x84)
  bytes.writeUInt16LE(0x2022, 0x80 + 22)
  return bytes
}

async function start(): Promise<{ server: TestServer; store: ReleaseStore }> {
  dir = mkdtempSync(join(tmpdir(), 'medicon-releases-'))
  const store = new ReleaseStore({
    dir,
    publicBaseUrl: 'https://medicon.example.com',
    signingKey: keys.privateKey,
    mcbeVersion: '1.26.52',
  })
  server = await startServer({ releases: store })
  return { server, store }
}

async function owner(s: TestServer, name = 'faokun'): Promise<string> {
  const token = await s.signUp(name)
  await s.admin('/users/rank', { name, rank: 'owner' })
  return token
}

async function upload(s: TestServer, token: string | null, body: Buffer, type = RELEASE_CONTENT_TYPE) {
  const response = await fetch(`${s.baseUrl}/releases`, {
    method: 'POST',
    headers: { 'Content-Type': type, ...(token ? { Authorization: `Bearer ${token}` } : {}) },
    body: new Uint8Array(body),
  })
  return { status: response.status, body: await response.json().catch(() => null) }
}

const release = (version: string, dll = fakeDll()) =>
  buildReleaseUpload({ version, title: `Medicon ${version}`, body: '+ Added auto update\n* Fixed Scaffold' }, dll)

async function bytes(url: string): Promise<{ status: number; data: Buffer }> {
  const response = await fetch(url)
  return { status: response.status, data: Buffer.from(await response.arrayBuffer()) }
}

describe('Core releases', () => {
  it('compares versions numerically', () => {
    expect(compareVersions('1.0.10', '1.0.9')).toBe(1)
    expect(compareVersions('1.2.0', '1.10.0')).toBe(-1)
    expect(compareVersions('2.0.0', '2.0.0')).toBe(0)
  })

  it('is disabled without a signing key', async () => {
    server = await startServer()
    const token = await owner(server)
    expect((await upload(server, token, release('1.0.0'))).body).toEqual({ error: 'releases_disabled' })
    expect((await bytes(`${server.baseUrl}/medicon/manifest.json`)).status).toBe(404)
  })

  it('publishes a signed manifest, the DLL and its Change Log in one request', async () => {
    const { server: s } = await start()
    const token = await owner(s)
    const watcher = await connect(s.wsUrl, await s.signUp('watcher'))
    // Far above the public server's 16 KiB body limit for other routes.
    const dll = fakeDll(0x42, 3 * 1024 * 1024)
    const result = await upload(s, token, release('1.0.5', dll))
    expect(result.status).toBe(200)
    expect(result.body.manifest).toMatchObject({
      version: '1.0.5',
      channel: 'stable',
      mcbe_version: '1.26.52',
      loader_api: 1,
      download_url: 'https://medicon.example.com/medicon/releases/1.0.5/MediconCore.dll',
      sha256: createHash('sha256').update(dll).digest('hex'),
      size: dll.length,
      changelog: ['+ Added auto update', '* Fixed Scaffold'],
    })

    const manifest = await bytes(`${s.baseUrl}/medicon/manifest.json`)
    const signature = await bytes(`${s.baseUrl}/medicon/manifest.sig`)
    expect(manifest.status).toBe(200)
    expect(JSON.parse(manifest.data.toString('utf8')).version).toBe('1.0.5')
    // The loader checks exactly this: RSA-PSS / SHA-256 / salt 32 over the raw bytes.
    const options = { key: keys.publicKey, padding: constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 }
    expect(verify('sha256', manifest.data, options, signature.data)).toBe(true)
    expect(verify('sha256', Buffer.concat([manifest.data, Buffer.from(' ')]), options, signature.data)).toBe(false)

    const core = await bytes(`${s.baseUrl}/medicon/releases/1.0.5/MediconCore.dll`)
    expect(core.status).toBe(200)
    expect(core.data.equals(dll)).toBe(true)
    const pinned = await bytes(`${s.baseUrl}/medicon/releases/1.0.5/manifest.json`)
    expect(pinned.data.equals(manifest.data)).toBe(true)

    const posted = await watcher.next((frame) => frame.t === 'news.new')
    expect(posted.item).toMatchObject({ kind: 'changelog', title: 'Medicon 1.0.5', version: '1.0.5' })
    const news = await http(`${s.baseUrl}/news?kind=changelog`, { headers: { Authorization: `Bearer ${token}` } })
    expect(news.body.items[0]).toMatchObject({ version: '1.0.5', body: '+ Added auto update\n* Fixed Scaffold' })
    watcher.ws.close()
  })

  it('accepts only owners, newer versions and x64 DLLs', async () => {
    const { server: s, store } = await start()
    const user = await s.signUp('someone')
    expect((await upload(s, null, release('1.0.0'))).status).toBe(401)
    expect((await upload(s, user, release('1.0.0'))).body).toEqual({ error: 'forbidden' })
    const token = await owner(s)
    expect((await upload(s, token, release('1.0.0'))).status).toBe(200)
    expect((await upload(s, token, release('1.0.0'))).body).toEqual({ error: 'version_conflict' })
    expect((await upload(s, token, release('0.9.9'))).body).toEqual({ error: 'version_conflict' })
    expect((await upload(s, token, release('1.0.1', Buffer.from('not a dll')))).body).toEqual({ error: 'bad_release' })
    const exe = fakeDll()
    exe.writeUInt16LE(0x0022, 0x80 + 22)
    expect((await upload(s, token, release('1.0.1', exe))).body).toEqual({ error: 'bad_release' })
    expect((await upload(s, token, Buffer.from('MDCRjunk'))).body).toEqual({ error: 'bad_release' })
    expect(store.latestVersion()).toBe('1.0.0')
    // No Change Log was posted for any refused upload.
    const news = await http(`${s.baseUrl}/news?kind=changelog`, { headers: { Authorization: `Bearer ${token}` } })
    expect(news.body.items).toHaveLength(1)
  })

  it('never resolves a version outside the releases directory', async () => {
    const { server: s } = await start()
    await upload(s, await owner(s), release('1.0.0'))
    for (const path of ['..%2F..%2Fpackage.json/MediconCore.dll', '1.0.0/..%2F..%2Fpackage.json', '1.0/MediconCore.dll', '1.0.0/other.dll'])
      expect((await bytes(`${s.baseUrl}/medicon/releases/${path}`)).status).toBe(404)
  })

  it('publishes from the admin port (release:publish)', async () => {
    const { server: s } = await start()
    const response = await fetch(`${s.adminUrl}/releases`, {
      method: 'POST',
      headers: { 'X-Admin-Secret': ADMIN_SECRET, 'Content-Type': RELEASE_CONTENT_TYPE },
      body: new Uint8Array(release('2.0.0')),
    })
    expect(response.status).toBe(200)
    expect((await bytes(`${s.baseUrl}/medicon/releases/2.0.0/MediconCore.dll`)).status).toBe(200)
  })

  it('parses only well-formed uploads', () => {
    const good = release('1.2.3')
    expect(parseReleaseUpload(good)?.metadata.version).toBe('1.2.3')
    expect(parseReleaseUpload(good.subarray(0, 10))).toBeNull()
    const badVersion = buildReleaseUpload({ version: '1.2', title: 't', body: 'b' }, fakeDll())
    expect(parseReleaseUpload(badVersion)).toBeNull()
    const traversal = buildReleaseUpload({ version: '../1.0.0', title: 't', body: 'b' }, fakeDll())
    expect(parseReleaseUpload(traversal)).toBeNull()
  })
})
