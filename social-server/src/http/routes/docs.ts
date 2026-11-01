import { readFileSync } from 'node:fs'
import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify'

const DOCS_HTML = readFileSync(new URL('../docs.html', import.meta.url))

// The public Script SDK docs: one self-contained page, so no sign-in or scripts.
export function registerDocsRoutes(app: FastifyInstance): void {
  const sendDocs = async (_request: FastifyRequest, reply: FastifyReply) =>
    reply
      .header('Cache-Control', 'public, max-age=300')
      .header('Content-Security-Policy', "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'")
      .header('X-Content-Type-Options', 'nosniff')
      .type('text/html; charset=utf-8')
      .send(DOCS_HTML)
  app.get('/docs', sendDocs)
  app.get('/docs/', sendDocs)
}
