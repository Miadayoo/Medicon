// Logs are limited to startup/shutdown, connection counts and errors.
// Never pass message bodies, license keys or tokens to these functions.

function line(level: string, message: string): string {
  return `${new Date().toISOString()} ${level} ${message}`
}

export const log = {
  info(message: string): void {
    console.log(line('INFO', message))
  },
  error(message: string, err?: unknown): void {
    const detail = err instanceof Error ? (err.stack ?? err.message) : err === undefined ? '' : String(err)
    console.error(line('ERROR', detail === '' ? message : `${message}: ${detail}`))
  },
}
