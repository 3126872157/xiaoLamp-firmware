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
  let edge = false, frame = 0, companion = false;
  page.on('pageerror', e => errors.push(e.message));
  await page.route('http://omni.test/**', async route => {
    const r = route.request();
    if (r.url().endsWith('/api/lamp/omni')) {
      assert.equal(r.headers()['x-lamp-token'], 'test-token');
      if (r.method() === 'POST') {const body=r.postDataJSON();operations.push(body);if(body.op==='companion')companion=body.enabled;}
      await route.fulfill({ json: { ok: true, configured: operations.length > 0, status: 'configured', cloud_motion_enabled: edge, manual_image_supported: edge, companion_enabled:companion } });
    } else if (r.url().endsWith('/api/lamp/vision/jpeg')) {
      assert.equal(r.headers()['x-lamp-token'], 'test-token');
      assert.equal(new URL(r.url()).search, '');
      await route.fulfill({contentType:'image/jpeg',body:fs.readFileSync(new URL('fixtures/omni/preview.jpg',import.meta.url))});
    } else if (r.url().endsWith('/api/lamp/vision')) {
      assert.equal(r.headers()['x-lamp-token'], 'test-token');
      if(r.method()==='POST'){assert.deepEqual(r.postDataJSON(),{op:'capture'});frame++;await route.fulfill({json:{ok:true,status:'accepted'}});}
      else await route.fulfill({json:{available:true,busy:false,frame_id:frame,width:16,height:12}});
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
  await page.locator('#vision-panel > summary').click();
  await page.locator('#vision-capture').click();
  await page.waitForFunction(()=>{const i=document.querySelector('#vision-preview');return i.complete&&i.naturalWidth===16});
  assert.equal(await page.locator('#vision-preview').evaluate(i=>i.naturalHeight),12);
  for(const op of ['vision_on','vision_off']){await page.locator(`[data-omni="${op}"]`).click();await page.waitForFunction(op=>!document.querySelector(`[data-omni="${op}"]`).disabled,op);}
  await page.locator('#vision-panel details summary').click();
  await page.locator('#companion-interval').fill('10');
  await page.locator('#companion-start').click();
  await page.waitForFunction(()=>!document.querySelector('#companion-start').disabled);
  assert.deepEqual(operations.at(-1),{op:'companion',enabled:true,actions:true,interval_s:10});
  await page.locator('#companion-actions').uncheck();
  await new Promise(r=>setTimeout(r,100));
  assert.equal(operations.at(-1).actions,false);
  await page.locator('#companion-actions').check();
  await page.waitForFunction(()=>document.querySelector('#omni-status').textContent.includes('configured'));
  await new Promise(r=>setTimeout(r,100));
  assert.equal(operations.at(-1).actions,true);
  await page.locator('#companion-stop').click();
  await page.waitForFunction(()=>!document.querySelector('#companion-stop').disabled);
  assert.equal(operations.at(-1).enabled,false);
  await page.locator('#observe-once').click();
  await page.waitForFunction(()=>!document.querySelector('#observe-once').disabled);
  assert.equal(operations.at(-1).op,'observe_once');
  assert.deepEqual(errors,[]);
  console.log('PASS mobile panel: token, configuration, cleared key and all controls; mocked HTTP');
} finally { await browser.close(); }
