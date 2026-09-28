import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { runInNewContext } from 'node:vm';

const html = readFileSync(new URL('../../src_assets/common/assets/web/config.html', import.meta.url), 'utf8');
// Exercise the page's actual serialization, Save and Apply methods without mounting Vue.
const methodsSource = html.slice(html.indexOf('      serialize() {'), html.indexOf('      handleSearch() {'));

test('Save serializes performance logging as a boolean and omits the disabled default', async () => {
  const bodies = [];
  const methods = runInNewContext(`({${methodsSource}})`, {
    apiFetch: async (url, options) => {
      bodies.push(JSON.parse(options.body));
      return { status: 200 };
    },
  });
  const state = {
    ...methods,
    config: { streaming_performance_logging: true },
    tabs: [{ options: { streaming_performance_logging: false } }],
  };
  assert.equal(await state.save(), true);
  assert.deepEqual(bodies[0], { streaming_performance_logging: true });
  assert.equal(state.config.streaming_performance_logging, true);
  state.config.streaming_performance_logging = false;
  assert.equal(await state.save(), true);
  assert.deepEqual(bodies[1], {});
  assert.equal(state.config.streaming_performance_logging, false);
});

test('Apply saves adaptive choices without changing form values and restarts', async () => {
  const requests = [];
  const methods = runInNewContext(`({${methodsSource}})`, {
    apiFetch: async (url, options) => {
      requests.push({ url, ...options });
      return { status: 200 };
    },
    setTimeout: () => {},
  });
  const config = {
    virtual_display: 'enabled',
    virtual_display_layout: 'adaptive',
    virtual_display_local_disconnect: 'remove',
    virtual_display_headless_disconnect: 'retain',
    virtual_display_retention_power: 'system',
    virtual_display_local_override: 'absent',
  };
  const state = {
    ...methods,
    config: { ...config },
    tabs: [{ options: {
      virtual_display_local_disconnect: 'remove',
      virtual_display_headless_disconnect: 'retain',
      virtual_display_retention_power: 'display',
      virtual_display_local_override: 'auto',
    } }],
    saved: false,
    restarted: false,
  };
  state.apply();
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(requests.map(request => request.url), ['./api/config', './api/restart']);
  assert.deepEqual(JSON.parse(requests[0].body), {
    virtual_display: 'enabled',
    virtual_display_layout: 'adaptive',
    virtual_display_retention_power: 'system',
    virtual_display_local_override: 'absent',
  });
  assert.deepEqual(state.config, config);
  assert.equal(state.saved, true);
  assert.equal(state.restarted, true);
});
