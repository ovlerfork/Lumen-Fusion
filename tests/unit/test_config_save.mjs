import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { runInNewContext } from 'node:vm';

const html = readFileSync(new URL('../../src_assets/common/assets/web/config.html', import.meta.url), 'utf8');
// Exercise the page's actual serialization, Save and Apply methods without mounting Vue.
const methodsSource = html.slice(html.indexOf('      serialize() {'), html.indexOf('      handleSearch() {'));

function page(retention) {
  const requests = [];
  const methods = runInNewContext(`({${methodsSource}})`, {
    apiFetch: async (url, options) => {
      requests.push({ url, ...options });
      return { status: 200 };
    },
    setTimeout: () => {},
  });
  return {
    requests,
    state: {
      ...methods,
      config: { virtual_display_retention_seconds: retention, virtual_display_layout: 'adaptive' },
      tabs: [{ options: { virtual_display_retention_seconds: 600 } }],
      saved: true,
      restarted: false,
      retentionError: '',
    },
  };
}

test('Save writes valid durations once in parser-compatible decimal form', async () => {
  for (const value of [0, 600, 2147483647, '0', '600', '2147483647', '00060', '-0']) {
    const { state, requests } = page(value);
    assert.equal(await state.save(), true);
    assert.equal(requests.length, 1);
    assert.equal(requests[0].url, './api/config');
    const body = JSON.parse(requests[0].body);
    assert.equal(body.virtual_display_retention_seconds ?? 600, Number(value) || 0);
    assert.equal(state.retentionError, '');
    assert.equal(state.config.virtual_display_retention_seconds, value);
  }
});

test('Save rejects invalid and blank durations without writing or losing edits', async () => {
  for (const value of [-1, 1.5, 2147483648, '-1', '1.5', '2147483648', '', ' ', '600s', '1e3', '0x10', null, undefined, NaN, Infinity]) {
    const { state, requests } = page(value);
    assert.equal(await state.save(), false);
    assert.equal(requests.length, 0);
    assert.equal(state.saved, false);
    assert.equal(state.retentionError, 'config.virtual_display_retention_seconds_error');
    assert.equal(state.config.virtual_display_retention_seconds, value);
  }
});

test('Apply does not save or restart invalid edits; correction clears the error', async () => {
  const { state, requests } = page(-1);
  state.apply();
  await Promise.resolve();
  assert.equal(requests.length, 0);
  assert.equal(state.restarted, false);
  assert.equal(state.retentionError, 'config.virtual_display_retention_seconds_error');
  state.config.virtual_display_retention_seconds = 0;
  assert.equal(await state.save(), true);
  assert.equal(requests.length, 1);
  assert.equal(state.retentionError, '');
});
