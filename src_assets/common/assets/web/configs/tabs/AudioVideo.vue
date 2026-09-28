<script setup>
import {ref} from 'vue'
import {$tp} from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input type="text" class="form-control" id="audio_sink"
             :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
             v-model="config.audio_sink" />
      <div class="form-text">
        {{ $tp('config.audio_sink_desc') }}<br>
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #freebsd>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>


    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input type="text" class="form-control" id="virtual_sink" :placeholder="$t('config.virtual_sink_placeholder')"
                 v-model="config.virtual_sink" />
          <div class="form-text">{{ $t('config.virtual_sink_desc') }}</div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <Checkbox class="mb-3"
                  id="install_steam_audio_drivers"
                  locale-prefix="config"
                  v-model="config.install_steam_audio_drivers"
                  default="true"
        ></Checkbox>
      </template>
      <template #macos>
        <div class="mb-3">
          <label for="macos_capture_queue_depth" class="form-label">{{ $t('config.macos_capture_queue_depth') }}</label>
          <input type="number" min="3" max="8" step="1" class="form-control" id="macos_capture_queue_depth"
                 placeholder="4" v-model="config.macos_capture_queue_depth" />
          <div class="form-text">{{ $t('config.macos_capture_queue_depth_desc') }}</div>
        </div>

        <!-- Virtual Display -->
        <div class="mb-3">
          <label for="virtual_display" class="form-label">{{ $t('config.virtual_display') }}</label>
          <select class="form-select" id="virtual_display" v-model="config.virtual_display">
            <option value="disabled">Disabled</option>
            <option value="enabled">Enabled</option>
          </select>
          <div class="form-text">{{ $t('config.virtual_display_desc') }}</div>
        </div>

        <!-- Virtual Display Layout -->
        <div class="mb-3" v-if="config.virtual_display === 'enabled'">
          <label for="virtual_display_layout" class="form-label">{{ $t('config.virtual_display_layout') }}</label>
          <select class="form-select" id="virtual_display_layout" v-model="config.virtual_display_layout">
            <option value="extend">{{ $t('config.virtual_display_layout_extend') }}</option>
            <option value="mirror">{{ $t('config.virtual_display_layout_mirror') }}</option>
            <option value="system">{{ $t('config.virtual_display_layout_system') }}</option>
            <option value="adaptive">{{ $t('config.virtual_display_layout_adaptive') }}</option>
            <option value="primary">{{ $t('config.virtual_display_layout_primary') }}</option>
          </select>
          <div class="form-text">{{ $t('config.virtual_display_layout_desc') }}</div>
        </div>
        <fieldset class="mb-3" v-if="config.virtual_display === 'enabled' && config.virtual_display_layout === 'adaptive'">
          <legend class="fs-6">{{ $t('config.virtual_display_retention') }}</legend>
          <p class="form-text">{{ $t('config.virtual_display_retention_desc') }}</p>
          <div class="mb-3">
            <label for="virtual_display_local_disconnect" class="form-label">{{ $t('config.virtual_display_local_disconnect') }}</label>
            <select class="form-select" id="virtual_display_local_disconnect" v-model="config.virtual_display_local_disconnect">
              <option value="remove">{{ $t('config.virtual_display_remove') }}</option>
              <option value="retain">{{ $t('config.virtual_display_retain') }}</option>
            </select>
          </div>
          <div class="mb-3">
            <label for="virtual_display_headless_disconnect" class="form-label">{{ $t('config.virtual_display_headless_disconnect') }}</label>
            <select class="form-select" id="virtual_display_headless_disconnect" v-model="config.virtual_display_headless_disconnect">
              <option value="retain">{{ $t('config.virtual_display_retain') }}</option>
              <option value="remove">{{ $t('config.virtual_display_remove') }}</option>
            </select>
          </div>
          <div class="mb-3">
            <label for="virtual_display_retention_power" class="form-label">{{ $t('config.virtual_display_retention_power') }}</label>
            <select class="form-select" id="virtual_display_retention_power" v-model="config.virtual_display_retention_power">
              <option value="display">{{ $t('config.virtual_display_retention_power_display') }}</option>
              <option value="system">{{ $t('config.virtual_display_retention_power_system') }}</option>
              <option value="none">{{ $t('config.virtual_display_retention_power_none') }}</option>
            </select>
            <div class="form-text">{{ $t('config.virtual_display_retention_power_desc') }}</div>
          </div>
          <div class="mb-3">
            <label for="virtual_display_local_override" class="form-label">{{ $t('config.virtual_display_local_override') }}</label>
            <select class="form-select" id="virtual_display_local_override" v-model="config.virtual_display_local_override">
              <option value="auto">{{ $t('config.virtual_display_local_override_auto') }}</option>
              <option value="present">{{ $t('config.virtual_display_local_override_present') }}</option>
              <option value="absent">{{ $t('config.virtual_display_local_override_absent') }}</option>
            </select>
            <div class="form-text">{{ $t('config.virtual_display_local_override_desc') }}</div>
          </div>
        </fieldset>
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <Checkbox class="mb-3"
              id="stream_audio"
              locale-prefix="config"
              v-model="config.stream_audio"
              default="true"
    ></Checkbox>

    <AdapterNameSelector
        :platform="platform"
        :config="config"
    />

    <DisplayOutputSelector
      :platform="platform"
      :config="config"
    />

    <DisplayDeviceOptions
      :platform="platform"
      :config="config"
    />

    <!-- Display Modes -->
    <DisplayModesSettings
        :platform="platform"
        :config="config"
    />

  </div>
</template>

<style scoped>
</style>
