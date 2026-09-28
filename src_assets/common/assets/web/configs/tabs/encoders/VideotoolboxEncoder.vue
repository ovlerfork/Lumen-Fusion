<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="videotoolbox-encoder" class="config-page">
    <div class="mb-3">
      <label for="vt_low_latency_rate_control" class="form-label">{{ $t('config.vt_low_latency_rate_control') }}</label>
      <select id="vt_low_latency_rate_control" class="form-select" v-model="config.vt_low_latency_rate_control">
        <option value="inherit">{{ $t('config.vt_preference_inherit') }}</option>
        <option value="auto">{{ $t('config.vt_low_latency_rate_control_auto') }}</option>
      </select>
      <div class="form-text">{{ $t('config.vt_low_latency_rate_control_desc') }}</div>
    </div>
    <div class="mb-3">
      <label for="vt_prio_speed" class="form-label">{{ $t('config.vt_prio_speed') }}</label>
      <select id="vt_prio_speed" class="form-select" v-model="config.vt_prio_speed">
        <option value="inherit">{{ $t('config.vt_preference_inherit') }}</option>
        <option value="enabled">{{ $t('_common.enabled') }}</option>
        <option value="disabled">{{ $t('_common.disabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.vt_prio_speed_desc') }}</div>
    </div>
    <div class="mb-3">
      <label for="vt_power_efficient" class="form-label">{{ $t('config.vt_power_efficient') }}</label>
      <select id="vt_power_efficient" class="form-select" v-model="config.vt_power_efficient">
        <option value="inherit">{{ $t('config.vt_preference_inherit') }}</option>
        <option value="enabled">{{ $t('_common.enabled') }}</option>
        <option value="disabled">{{ $t('_common.disabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.vt_power_efficient_desc') }}</div>
    </div>
    <!-- Presets -->
    <div class="mb-3">
      <label for="vt_coder" class="form-label">{{ $t('config.vt_coder') }}</label>
      <select id="vt_coder" class="form-select" v-model="config.vt_coder">
        <option value="auto">{{ $t('config.ffmpeg_auto') }}</option>
        <option value="cabac">{{ $t('config.coder_cabac') }}</option>
        <option value="cavlc">{{ $t('config.coder_cavlc') }}</option>
      </select>
    </div>
    <div class="mb-3">
      <label for="vt_software" class="form-label">{{ $t('config.vt_software') }}</label>
      <select id="vt_software" class="form-select" v-model="config.vt_software">
        <option value="auto">{{ $t('_common.auto') }}</option>
        <option value="disabled">{{ $t('_common.disabled') }}</option>
        <option value="allowed">{{ $t('config.vt_software_allowed') }}</option>
        <option value="forced">{{ $t('config.vt_software_forced') }}</option>
      </select>
    </div>
    <Checkbox class="mb-3"
              id="vt_realtime"
              desc=""
              locale-prefix="config"
              v-model="config.vt_realtime"
              default="true"
    ></Checkbox>
    <div class="mb-3">
      <label for="vt_max_frame_delay" class="form-label">{{ $t('config.vt_max_frame_delay') }}</label>
      <input type="number" min="-1" class="form-control" id="vt_max_frame_delay" placeholder="-1"
             v-model="config.vt_max_frame_delay" />
      <div class="form-text">{{ $t('config.vt_max_frame_delay_desc') }}</div>
    </div>
  </div>
</template>

<style scoped>

</style>
