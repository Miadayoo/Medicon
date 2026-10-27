#!/bin/sh
set -eu

# Resolve paths relative to this file, even when launched from another directory.
SERVER_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SERVER_DIR"

fail() {
  printf '%s\n' "$1" >&2
  exit 1
}

command -v node >/dev/null 2>&1 || fail 'Node.js 24 or later is required. Install Node.js on this server first.'
node -e 'process.exit(Number(process.versions.node.split(".")[0]) >= 24 ? 0 : 1)' \
  || fail "Node.js 24 or later is required (found $(node --version))."

# Keep transferred settings. Generate a private admin secret only on first use.
node --input-type=module <<'NODE'
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { randomBytes } from 'node:crypto';

if (!existsSync('.env')) {
  const template = readFileSync('.env.example', 'utf8');
  const config = template.replace(/^ADMIN_SECRET=\r?$/m,
    'ADMIN_SECRET=' + randomBytes(32).toString('base64url'));
  writeFileSync('.env', config, { flag: 'wx', mode: 0o600 });
  console.log('Created .env with a random ADMIN_SECRET.');
}
NODE

# Also catches native SQLite modules copied from Windows or another Node version.
if ! node --input-type=module -e '
  import Database from "better-sqlite3";
  import "fastify";
  import "ws";
  import "zod";
  const db = new Database(":memory:");
  db.close();
' >/dev/null 2>&1; then
  command -v npm >/dev/null 2>&1 || fail 'npm is required to install dependencies. Install npm and try again.'
  printf '%s\n' 'Installing server dependencies...'
  npm ci --omit=dev --no-audit --no-fund
fi

printf '%s\n' "Starting Medicon Social server in $SERVER_DIR"
# Run directly so Ctrl+C and service-manager signals reach the server.
exec node src/index.ts
