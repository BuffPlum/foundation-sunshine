<script setup>
import { computed, onMounted, ref } from 'vue'
import ConfirmDialog from '../../components/common/ConfirmDialog.vue'
import {
  FILE_MAPPING_MODE_FULL_DISK,
  FILE_MAPPING_MODE_READ_ONLY,
  getFileMappingMode,
  updateFileMappingMode,
} from '../../utils/fileMappingMode.js'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
const fileMappingMode = ref(FILE_MAPPING_MODE_READ_ONLY)
const modeLoading = ref(true)
const modeSaving = ref(false)
const modeError = ref('')
const modeNotice = ref('')
const modeLoaded = ref(false)
const showFullDiskConfirm = ref(false)
const fullDiskAcknowledged = ref(false)

const fullDiskEnabled = computed(() => fileMappingMode.value === FILE_MAPPING_MODE_FULL_DISK)
const modeReady = computed(() => modeLoaded.value && !modeLoading.value && !modeError.value)

const errorMessage = (error) => {
  const message = error instanceof Error ? error.message : String(error || '')
  return message.trim() || 'Unknown error'
}

const loadFileMappingMode = async () => {
  modeLoading.value = true
  modeLoaded.value = false
  modeError.value = ''
  modeNotice.value = ''
  try {
    const result = await getFileMappingMode()
    fileMappingMode.value = result.mode
    modeLoaded.value = true
  } catch (error) {
    modeError.value = errorMessage(error)
  } finally {
    modeLoading.value = false
  }
}

const requestFileMappingMode = async (mode) => {
  if (!modeReady.value || modeSaving.value || mode === fileMappingMode.value) return

  modeNotice.value = ''
  modeError.value = ''
  if (mode === FILE_MAPPING_MODE_FULL_DISK) {
    fullDiskAcknowledged.value = false
    showFullDiskConfirm.value = true
    return
  }

  await persistFileMappingMode(FILE_MAPPING_MODE_READ_ONLY)
}

const persistFileMappingMode = async (mode) => {
  modeSaving.value = true
  modeError.value = ''
  modeNotice.value = ''
  try {
    const result = await updateFileMappingMode(mode)
    fileMappingMode.value = result.mode
    modeNotice.value = result.requiresStreamReconnect
      ? 'config.file_mapping_mode.saved_reconnect'
      : 'config.file_mapping_mode.saved'
    showFullDiskConfirm.value = false
    fullDiskAcknowledged.value = false
  } catch (error) {
    modeError.value = errorMessage(error)
  } finally {
    modeSaving.value = false
  }
}

const cancelFullDiskConfirm = () => {
  if (modeSaving.value) return
  showFullDiskConfirm.value = false
  fullDiskAcknowledged.value = false
}

const confirmFullDiskMode = async () => {
  if (!fullDiskAcknowledged.value || modeSaving.value) return
  await persistFileMappingMode(FILE_MAPPING_MODE_FULL_DISK)
}

onMounted(loadFileMappingMode)
</script>

<template>
  <div id="files" class="config-page">
    <section class="file-mapping-mode-panel" :class="{ 'is-full-disk': fullDiskEnabled }">
      <div class="file-mapping-mode-heading">
        <div>
          <h2>{{ $t('config.file_mapping_mode.title') }}</h2>
          <p>{{ $t('config.file_mapping_mode.description') }}</p>
        </div>
        <span class="mode-status" :class="modeError ? 'unknown' : (fullDiskEnabled ? 'danger' : 'safe')">
          <i :class="modeError
            ? 'fas fa-circle-question'
            : (fullDiskEnabled ? 'fas fa-exclamation-triangle' : 'fas fa-shield-alt')"></i>
          {{ modeLoading
            ? $t('config.file_mapping_mode.loading')
            : modeError
              ? $t('config.file_mapping_mode.unknown_badge')
              : $t(fullDiskEnabled
                ? 'config.file_mapping_mode.full_disk_badge'
                : 'config.file_mapping_mode.read_only_badge') }}
        </span>
      </div>

      <div class="mode-options" role="group" :aria-label="$t('config.file_mapping_mode.title')">
        <button
          type="button"
          class="mode-option"
          :class="{ active: modeReady && !fullDiskEnabled }"
          :aria-pressed="modeReady && !fullDiskEnabled"
          :disabled="!modeReady || modeSaving || !fullDiskEnabled"
          @click="requestFileMappingMode(FILE_MAPPING_MODE_READ_ONLY)"
        >
          <span class="mode-option-icon safe"><i class="fas fa-folder-open"></i></span>
          <span class="mode-option-copy">
            <strong>{{ $t('config.file_mapping_mode.read_only_title') }}</strong>
            <small>{{ $t('config.file_mapping_mode.read_only_desc') }}</small>
          </span>
          <i v-if="modeReady && !fullDiskEnabled" class="fas fa-check mode-selected"></i>
        </button>

        <button
          type="button"
          class="mode-option danger-option"
          :class="{ active: modeReady && fullDiskEnabled }"
          :aria-pressed="modeReady && fullDiskEnabled"
          :disabled="!modeReady || modeSaving || fullDiskEnabled"
          @click="requestFileMappingMode(FILE_MAPPING_MODE_FULL_DISK)"
        >
          <span class="mode-option-icon danger"><i class="fas fa-hard-drive"></i></span>
          <span class="mode-option-copy">
            <strong>{{ $t('config.file_mapping_mode.full_disk_title') }}</strong>
            <small>{{ $t('config.file_mapping_mode.full_disk_desc') }}</small>
          </span>
          <i v-if="modeReady && fullDiskEnabled" class="fas fa-check mode-selected"></i>
        </button>
      </div>

      <div v-if="modeSaving" class="mode-feedback info" role="status">
        <i class="fas fa-spinner fa-spin"></i>
        {{ $t('config.file_mapping_mode.saving') }}
      </div>
      <div v-else-if="modeError" class="mode-feedback danger" role="alert">
        <i class="fas fa-circle-exclamation"></i>
        <span>{{ $t('config.file_mapping_mode.failed', { error: modeError }) }}</span>
        <button type="button" class="btn btn-sm btn-outline-danger" @click="loadFileMappingMode">
          {{ $t('config.file_mapping_mode.retry') }}
        </button>
      </div>
      <div v-else-if="modeNotice" class="mode-feedback success" role="status">
        <i class="fas fa-circle-check"></i>
        {{ $t(modeNotice) }}
      </div>
      <div v-else-if="fullDiskEnabled" class="mode-feedback danger" role="alert">
        <i class="fas fa-triangle-exclamation"></i>
        {{ $t('config.file_mapping_mode.active_warning') }}
      </div>
    </section>

    <div class="settings-grid">
      <!-- Apps File -->
      <div class="settings-field">
        <label for="file_apps" class="form-label">{{ $t('config.file_apps') }}</label>
        <input
          type="text"
          class="form-control font-monospace"
          id="file_apps"
          placeholder="apps.json"
          v-model="config.file_apps"
        />
        <div class="form-text">{{ $t('config.file_apps_desc') }}</div>
      </div>

      <!-- Credentials File -->
      <div class="settings-field">
        <label for="credentials_file" class="form-label">{{ $t('config.credentials_file') }}</label>
        <input
          type="text"
          class="form-control font-monospace"
          id="credentials_file"
          placeholder="sunshine_state.json"
          v-model="config.credentials_file"
        />
        <div class="form-text">{{ $t('config.credentials_file_desc') }}</div>
      </div>

      <!-- Log Path -->
      <div class="settings-field">
        <label for="log_path" class="form-label">{{ $t('config.log_path') }}</label>
        <input
          type="text"
          class="form-control font-monospace"
          id="log_path"
          placeholder="sunshine.log"
          v-model="config.log_path"
        />
        <div class="form-text">{{ $t('config.log_path_desc') }}</div>
      </div>

      <!-- Private Key -->
      <div class="settings-field">
        <label for="pkey" class="form-label">{{ $t('config.pkey') }}</label>
        <input
          type="text"
          class="form-control font-monospace"
          id="pkey"
          placeholder="/dir/pkey.pem"
          v-model="config.pkey"
        />
        <div class="form-text">{{ $t('config.pkey_desc') }}</div>
      </div>

      <!-- Certificate -->
      <div class="settings-field">
        <label for="cert" class="form-label">{{ $t('config.cert') }}</label>
        <input
          type="text"
          class="form-control font-monospace"
          id="cert"
          placeholder="/dir/cert.pem"
          v-model="config.cert"
        />
        <div class="form-text">{{ $t('config.cert_desc') }}</div>
      </div>

      <!-- State File -->
      <div class="settings-field">
        <label for="file_state" class="form-label">{{ $t('config.file_state') }}</label>
        <input
          type="text"
          class="form-control font-monospace"
          id="file_state"
          placeholder="sunshine_state.json"
          v-model="config.file_state"
        />
        <div class="form-text">{{ $t('config.file_state_desc') }}</div>
      </div>
    </div>

    <ConfirmDialog
      :show="showFullDiskConfirm"
      dialog-id="full-disk-mode-confirm"
      :title="$t('config.file_mapping_mode.confirm_title')"
      title-icon="fas fa-triangle-exclamation"
      tone="danger"
      max-width="680px"
      :dismissible="!modeSaving"
      :close-label="$t('_common.close')"
      @close="cancelFullDiskConfirm"
    >
      <p class="full-disk-confirm-intro">
        {{ $t('config.file_mapping_mode.confirm_intro') }}
      </p>
      <ul class="full-disk-risk-list">
        <li>{{ $t('config.file_mapping_mode.confirm_all_drives') }}</li>
        <li>{{ $t('config.file_mapping_mode.confirm_write_delete') }}</li>
        <li>{{ $t('config.file_mapping_mode.confirm_trusted_lan') }}</li>
        <li>{{ $t('config.file_mapping_mode.confirm_backup') }}</li>
      </ul>
      <div v-if="modeError" class="full-disk-confirm-error" role="alert">
        <i class="fas fa-circle-exclamation"></i>
        {{ $t('config.file_mapping_mode.failed', { error: modeError }) }}
      </div>
      <label class="full-disk-acknowledgement">
        <input v-model="fullDiskAcknowledged" type="checkbox" :disabled="modeSaving" />
        <span>{{ $t('config.file_mapping_mode.confirm_checkbox') }}</span>
      </label>

      <template #actions>
        <button type="button" class="btn btn-secondary" :disabled="modeSaving" @click="cancelFullDiskConfirm">
          {{ $t('_common.cancel') }}
        </button>
        <button
          type="button"
          class="btn btn-danger"
          :disabled="!fullDiskAcknowledged || modeSaving"
          @click="confirmFullDiskMode"
        >
          <i v-if="modeSaving" class="fas fa-spinner fa-spin me-1"></i>
          {{ $t('config.file_mapping_mode.confirm_enable') }}
        </button>
      </template>
    </ConfirmDialog>
  </div>
</template>

<style scoped>
.file-mapping-mode-panel {
  margin-bottom: 1.25rem;
  padding: 1.25rem;
  border: 1px solid var(--ui-success-border);
  border-radius: var(--ui-radius-lg);
  background: linear-gradient(135deg, var(--ui-success-soft), transparent 72%);
}

.file-mapping-mode-panel.is-full-disk {
  border-color: var(--ui-danger-border);
  background: linear-gradient(135deg, var(--ui-danger-soft), transparent 72%);
}

.file-mapping-mode-heading {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
  margin-bottom: 1rem;
}

.file-mapping-mode-heading h2 {
  margin: 0 0 0.35rem;
  color: var(--ui-text-primary);
  font-size: 1.15rem;
}

.file-mapping-mode-heading p {
  margin: 0;
  color: var(--ui-text-secondary);
  line-height: 1.55;
}

.mode-status {
  display: inline-flex;
  flex: 0 0 auto;
  align-items: center;
  gap: 0.4rem;
  padding: 0.35rem 0.65rem;
  border: 1px solid;
  border-radius: 999px;
  font-size: 0.82rem;
  font-weight: 600;
  white-space: nowrap;
}

.mode-status.safe {
  color: var(--ui-success-text);
  border-color: var(--ui-success-border);
  background: var(--ui-success-soft);
}

.mode-status.danger {
  color: var(--ui-danger-text);
  border-color: var(--ui-danger-border);
  background: var(--ui-danger-soft);
}

.mode-status.unknown {
  color: var(--ui-warning-text);
  border-color: var(--ui-warning-border);
  background: var(--ui-warning-soft);
}

.mode-options {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 0.85rem;
}

.mode-option {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr) auto;
  align-items: center;
  gap: 0.8rem;
  min-height: 88px;
  padding: 0.9rem 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
  color: var(--ui-text-primary);
  text-align: left;
  transition: border-color 0.18s ease, box-shadow 0.18s ease, transform 0.18s ease;
}

.mode-option:not(:disabled) {
  cursor: pointer;
}

.mode-option:not(:disabled):hover,
.mode-option:not(:disabled):focus-visible {
  border-color: var(--ui-accent);
  box-shadow: 0 0 0 3px var(--ui-accent-soft);
  outline: none;
  transform: translateY(-1px);
}

.mode-option.active {
  border-color: var(--ui-success);
  box-shadow: 0 0 0 2px var(--ui-success-soft);
}

.mode-option.danger-option.active {
  border-color: var(--ui-danger);
  box-shadow: 0 0 0 2px var(--ui-danger-soft);
}

.mode-option:disabled:not(.active) {
  opacity: 0.58;
}

.mode-option-icon {
  width: 2.4rem;
  height: 2.4rem;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: 0.75rem;
}

.mode-option-icon.safe {
  color: var(--ui-success-text);
  background: var(--ui-success-soft);
}

.mode-option-icon.danger {
  color: var(--ui-danger-text);
  background: var(--ui-danger-soft);
}

.mode-option-copy {
  min-width: 0;
}

.mode-option-copy strong,
.mode-option-copy small {
  display: block;
}

.mode-option-copy small {
  margin-top: 0.3rem;
  color: var(--ui-text-secondary);
  line-height: 1.45;
}

.mode-selected {
  color: var(--ui-success-text);
}

.danger-option .mode-selected {
  color: var(--ui-danger-text);
}

.mode-feedback {
  display: flex;
  align-items: center;
  gap: 0.55rem;
  margin-top: 0.9rem;
  padding: 0.7rem 0.8rem;
  border: 1px solid;
  border-radius: var(--ui-radius-sm);
  line-height: 1.45;
}

.mode-feedback.info {
  color: var(--ui-text-secondary);
  border-color: var(--ui-border);
  background: var(--ui-surface);
}

.mode-feedback.success {
  color: var(--ui-success-text);
  border-color: var(--ui-success-border);
  background: var(--ui-success-soft);
}

.mode-feedback.danger {
  color: var(--ui-danger-text);
  border-color: var(--ui-danger-border);
  background: var(--ui-danger-soft);
}

.mode-feedback .btn {
  margin-left: auto;
}

.full-disk-confirm-intro {
  color: var(--ui-text-primary);
  font-weight: 600;
}

.full-disk-risk-list {
  margin: 1rem 0;
  padding: 1rem 1rem 1rem 2.25rem;
  border: 1px solid var(--ui-danger-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-danger-soft);
  color: var(--ui-danger-text);
}

.full-disk-risk-list li + li {
  margin-top: 0.55rem;
}

.full-disk-acknowledgement {
  display: flex;
  align-items: flex-start;
  gap: 0.7rem;
  padding: 0.9rem;
  border: 1px solid var(--ui-warning-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-warning-soft);
  color: var(--ui-text-primary);
  cursor: pointer;
}

.full-disk-confirm-error {
  display: flex;
  align-items: flex-start;
  gap: 0.55rem;
  margin-bottom: 0.9rem;
  padding: 0.8rem 0.9rem;
  border: 1px solid var(--ui-danger-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-danger-soft);
  color: var(--ui-danger-text);
}

.full-disk-acknowledgement input {
  width: 1.1rem;
  height: 1.1rem;
  flex: 0 0 auto;
  margin-top: 0.18rem;
  accent-color: var(--ui-danger);
}

@media (max-width: 767.98px) {
  .file-mapping-mode-heading {
    flex-direction: column;
  }

  .mode-options {
    grid-template-columns: 1fr;
  }

  .mode-status {
    white-space: normal;
  }
}
</style>
