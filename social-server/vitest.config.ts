import { defineConfig } from 'vitest/config'

export default defineConfig({
  test: {
    include: ['tests/**/*.test.ts'],
    // The hub logs connection counts; only show that output for failing tests.
    silent: 'passed-only',
  },
})
