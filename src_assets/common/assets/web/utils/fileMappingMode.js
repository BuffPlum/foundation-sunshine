import { apiJson } from './apiFetch.js'

export const FILE_MAPPING_MODE_READ_ONLY = 'read_only'
export const FILE_MAPPING_MODE_FULL_DISK = 'full_disk'

const VALID_FILE_MAPPING_MODES = new Set([
  FILE_MAPPING_MODE_READ_ONLY,
  FILE_MAPPING_MODE_FULL_DISK,
])

export const normalizeFileMappingMode = (mode) => {
  const normalized = String(mode || '').trim().toLowerCase()
  if (!VALID_FILE_MAPPING_MODES.has(normalized)) {
    throw new Error(`Unsupported file mapping mode: ${mode}`)
  }
  return normalized
}

const parseModeResponse = (data) => {
  if (!data?.ok) {
    throw new Error(data?.error || 'Sunshine did not accept the file mapping mode request')
  }

  return {
    mode: normalizeFileMappingMode(data.mode),
    requiresStreamReconnect: data.requires_stream_reconnect !== false,
  }
}

export const getFileMappingMode = async () => {
  const data = await apiJson('/api/v1/file-mapping/mode')
  return parseModeResponse(data)
}

export const updateFileMappingMode = async (mode) => {
  const normalized = normalizeFileMappingMode(mode)
  const data = await apiJson('/api/v1/file-mapping/mode', {
    method: 'PATCH',
    body: { mode: normalized },
  })
  return parseModeResponse(data)
}
