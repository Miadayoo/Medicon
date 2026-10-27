import { generateKeyPairSync } from 'node:crypto'
import { existsSync, mkdirSync, writeFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { parseArgs } from 'node:util'
import { ROOT_DIR } from '../src/config.ts'
import { publicKeyHeader } from './release-setup.ts'

// Creates the Core release signing key pair (docs/auto-update.md).
// The private key is written here only, never to the repository's include/.
// The public key becomes include/medicon/update/ReleasePublicKey.hpp, which
// MediconLoader embeds. Rebuild and redistribute the loader after rotating.
const { values } = parseArgs({
  options: {
    out: { type: 'string', default: 'keys/release-private.pem' },
    header: { type: 'string', default: '../include/medicon/update/ReleasePublicKey.hpp' },
    force: { type: 'boolean', default: false },
  },
})
const privatePath = resolve(ROOT_DIR, values.out)
const headerPath = resolve(ROOT_DIR, values.header)
if (existsSync(privatePath) && !values.force) {
  console.error(`既に鍵があります: ${privatePath}（作り直すなら --force。既存の Loader は新しい鍵の署名を受け付けません）`)
  process.exit(1)
}

const { privateKey } = generateKeyPairSync('rsa', { modulusLength: 3072, publicExponent: 65537 })
mkdirSync(dirname(privatePath), { recursive: true })
writeFileSync(privatePath, privateKey.export({ type: 'pkcs8', format: 'pem' }), { mode: 0o600 })

mkdirSync(dirname(headerPath), { recursive: true })
writeFileSync(headerPath, publicKeyHeader(privateKey))
console.log(`秘密鍵: ${privatePath}（このサーバーの外へ出さない。.env の RELEASE_SIGNING_KEY に設定）`)
console.log(`公開鍵ヘッダー: ${headerPath}（コミットして MediconLoader をビルドし直す）`)
