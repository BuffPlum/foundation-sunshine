import test from 'node:test'
import assert from 'node:assert/strict'

import {
  FILE_MAPPING_MODE_FULL_DISK,
  FILE_MAPPING_MODE_READ_ONLY,
  getFileMappingMode,
  normalizeFileMappingMode,
  updateFileMappingMode,
} from '../utils/fileMappingMode.js'

const withMockFetch = async (handler, run) => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = handler
  try {
    await run()
  } finally {
    globalThis.fetch = originalFetch
  }
}

test('normalizes only the two supported file mapping modes', () => {
  assert.equal(normalizeFileMappingMode(' READ_ONLY '), FILE_MAPPING_MODE_READ_ONLY)
  assert.equal(normalizeFileMappingMode('full_disk'), FILE_MAPPING_MODE_FULL_DISK)
  assert.throws(() => normalizeFileMappingMode('readwrite'), /Unsupported file mapping mode/)
})

test('loads the active mode from the dedicated management endpoint', async () => {
  await withMockFetch(
    async (url, options) => {
      assert.equal(url, '/api/v1/file-mapping/mode')
      assert.equal(options.method, undefined)
      return new Response(JSON.stringify({
        ok: true,
        mode: 'read_only',
        requires_stream_reconnect: true,
      }), { status: 200 })
    },
    async () => {
      assert.deepEqual(await getFileMappingMode(), {
        mode: FILE_MAPPING_MODE_READ_ONLY,
        requiresStreamReconnect: true,
      })
    },
  )
})

test('persists full-disk mode with an explicit PATCH request', async () => {
  await withMockFetch(
    async (url, options) => {
      assert.equal(url, '/api/v1/file-mapping/mode')
      assert.equal(options.method, 'PATCH')
      assert.equal(options.headers.get('Content-Type'), 'application/json')
      assert.equal(options.body, JSON.stringify({ mode: 'full_disk' }))
      return new Response(JSON.stringify({
        ok: true,
        mode: 'full_disk',
        requires_stream_reconnect: true,
      }), { status: 200 })
    },
    async () => {
      assert.deepEqual(await updateFileMappingMode(FILE_MAPPING_MODE_FULL_DISK), {
        mode: FILE_MAPPING_MODE_FULL_DISK,
        requiresStreamReconnect: true,
      })
    },
  )
})

test('does not send unsupported modes to Sunshine', async () => {
  let called = false
  await withMockFetch(
    async () => {
      called = true
      return new Response('{}', { status: 200 })
    },
    async () => {
      await assert.rejects(() => updateFileMappingMode('readwrite'), /Unsupported file mapping mode/)
      assert.equal(called, false)
    },
  )
})
