import { createHash } from 'node:crypto'
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { parseArgs } from 'node:util'
import {
  CORE_FILE,
  LOADER_API,
  VERSION_PATTERN,
  changelogLines,
  isX64Dll,
  loadSigningKey,
  signManifest,
  type Manifest,
} from '../src/services/releases.ts'

// Static hosting without the server (docs/auto-update.md): writes
// <out>/medicon/{manifest.json,manifest.sig,releases/<v>/MediconCore.dll}
// for any static file host on the loader's allowlist.
const { values } = parseArgs({
  options: {
    file: { type: 'string' },
    version: { type: 'string' },
    key: { type: 'string' },
    'base-url': { type: 'string' },
    'changelog-file': { type: 'string' },
    'mcbe-version': { type: 'string', default: '1.26.60' },
    out: { type: 'string', default: 'out/static' },
  },
})
const need = (value: string | undefined, flag: string): string => {
  if (!value) {
    console.error(`${flag} を指定してください`)
    process.exit(1)
  }
  return value
}
const cwd = process.env.INIT_CWD ?? process.cwd()
const version = need(values.version, '--version')
if (!VERSION_PATTERN.test(version)) throw new Error('--version は x.y.z')
const dll = readFileSync(resolve(cwd, need(values.file, '--file')))
if (!isX64Dll(dll)) throw new Error('x64 DLL ではありません')
const baseUrl = need(values['base-url'], '--base-url').replace(/\/+$/, '')
if (!/^https:\/\/[a-z0-9.-]+(:\d{1,5})?$/i.test(baseUrl)) throw new Error('--base-url は https://host')
const manifest: Manifest = {
  version,
  channel: 'stable',
  mcbe_version: values['mcbe-version'] ?? '1.26.60',
  loader_api: LOADER_API,
  download_url: `${baseUrl}/medicon/releases/${version}/${CORE_FILE}`,
  sha256: createHash('sha256').update(dll).digest('hex'),
  size: dll.length,
  changelog: values['changelog-file'] ? changelogLines(readFileSync(resolve(cwd, values['changelog-file']), 'utf8')) : [],
  published_at: Date.now(),
}
const bytes = Buffer.from(JSON.stringify(manifest, null, 2) + '\n', 'utf8')
const signature = signManifest(loadSigningKey(resolve(cwd, need(values.key, '--key'))), bytes)
const root = resolve(cwd, values.out ?? 'out/static', 'medicon')
const versionDir = join(root, 'releases', version)
mkdirSync(versionDir, { recursive: true })
copyFileSync(resolve(cwd, values.file ?? ''), join(versionDir, CORE_FILE))
for (const dir of [root, versionDir]) {
  writeFileSync(join(dir, 'manifest.json'), bytes)
  writeFileSync(join(dir, 'manifest.sig'), signature)
}
console.log(`${root} に ${version} を書き出しました (sha256 ${manifest.sha256})`)
