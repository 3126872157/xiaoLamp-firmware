// Uses the parent web-sim's existing Playwright dependency. Mock HTTP only; no cloud/key.
import fs from 'node:fs';
import assert from 'node:assert/strict';
const { chromium } = await import(new URL('../../../../web-sim/node_modules/playwright-core/index.mjs', import.meta.url));
const header = fs.readFileSync(new URL('../../main/boards/waveshare/apple-lamp-ov3660/lamp_omni_page.h', import.meta.url), 'utf8');
const fragment = header.match(/R"omnipage\(([\s\S]*?)\)omnipage"/)[1];
const browser = await chromium.launch({ executablePath: process.env.CHROME_PATH ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true });
try {
  const page = await browser.newPage({ viewport: { width: 390, height: 844 } });
  const errors = [], operations = [];
  let edge = false;
  page.on('pageerror', e => errors.push(e.message));
  await page.route('http://omni.test/**', async route => {
    const r = route.request();
    if (r.url().endsWith('/api/lamp/omni')) {
      assert.equal(r.headers()['x-lamp-token'], 'test-token');
      if (r.method() === 'POST') operations.push(r.postDataJSON());
      await route.fulfill({ json: { ok: true, configured: operations.length > 0, status: 'configured', cloud_motion_enabled: edge } });
    } else if (r.url().endsWith('/api/lamp/runtime')) {
      assert.equal(r.headers()['x-lamp-token'], 'test-token');
      await route.fulfill({json:{runtime:{stage:'semantic_tools',vision_enabled:false}}});
    } else {
      await route.fulfill({ contentType: 'text/html', body: `<html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><script>const token='test-token';</script>${fragment}</html>` });
    }
  });
  await page.goto('http://omni.test/');
  await page.locator('#omni-workspace').fill('test-space');
  await page.locator('#omni-key').fill('dummy-only-key');
  await page.locator('[data-omni="configure"]').click();
  await page.waitForFunction(() => document.querySelector('#omni-key').value === '');
  assert.deepEqual(operations[0], {op:'configure',workspace:'test-space',region:'cn-beijing',key:'dummy-only-key'});
  for (const op of ['start','finish','vad','cancel','close','clear']) {
    await page.locator(`[data-omni="${op}"]`).click();
    await page.waitForFunction(op => !document.querySelector(`[data-omni="${op}"]`).disabled,op);
  }
  assert.deepEqual(operations.slice(1).map(o => o.op), ['start','finish','vad','cancel','close','clear']);
  assert.equal((await page.locator('#omni-status').innerText()).includes('dummy-only-key'), false);
  assert.match(await page.locator('#omni-capability').innerText(), /PoC/);
  edge = true; await page.reload();
  await page.waitForFunction(()=>document.querySelector('#runtime-status').textContent.includes('semantic_tools'));
  assert.match(await page.locator('#omni-capability').innerText(), /退出网页手动控制/);
  assert.deepEqual(errors,[]);
  console.log('PASS mobile panel: token, configuration, cleared key and all controls; mocked HTTP');
} finally { await browser.close(); }
