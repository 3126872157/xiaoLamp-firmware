// Production C++ semantic adapter -> existing HTTP relay -> real URDF browser executor.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { createRelay } from '../../../web-sim/scripts/bridge-relay.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const web = path.resolve(root, '../../web-sim');
const requireWeb = createRequire(path.join(web, 'package.json'));
const { chromium } = requireWeb('playwright-core');
const binary = path.join(root, 'build', process.platform === 'win32' ? 'lamp_protocol_cli.exe' : 'lamp_protocol_cli');
const token = randomUUID();
const relay = createRelay({ token });
await new Promise(resolve => relay.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${relay.address().port}`;
const headers = { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` };
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
function adapter(value, mode = 'sequence') {
  const r = spawnSync(binary, [mode], { input: JSON.stringify(value), encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr); return JSON.parse(r.stdout);
}
async function http(route, body) {
  const r = await fetch(base + route, { headers, method: body ? 'POST' : 'GET', body: body ? JSON.stringify(body) : undefined, signal: AbortSignal.timeout(4000) });
  return { status: r.status, data: await r.json() };
}
async function submit(command) {
  const submitted = await http('/v1/commands', command);
  assert.ok([200, 202].includes(submitted.status), JSON.stringify(submitted.data));
  for (let i = 0; i < 80; ++i) {
    const record = (await http(`/v1/requests/${encodeURIComponent(command.request_id)}`)).data;
    if (record.delivery === 'answered') return record.response;
    await sleep(100);
  }
  throw new Error(`No application receipt for ${command.request_id}`);
}
function query(operation, args = {}) {
  return adapter({ protocol_version: 'lamp-action/2', request_id: randomUUID(), operation, args }, 'command');
}
async function waitTask(id) {
  for (let i = 0; i < 100; ++i) {
    const r = await submit(query('get_task', { task_id: id }));
    assert.equal(r.ok, true, JSON.stringify(r));
    if (['completed', 'limited', 'failed', 'cancelled', 'blocked'].includes(r.result.status)) return r.result;
    await sleep(100);
  }
  throw new Error('Task timeout');
}
const browser = await chromium.launch({ executablePath: process.env.CHROME_PATH ?? 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true,
  args: ['--enable-unsafe-swiftshader', '--use-gl=angle', '--use-angle=swiftshader'] });
try {
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  const errors = []; page.on('pageerror', e => errors.push(e.message));
  await page.goto(`${process.env.VERIFY_URL ?? 'http://localhost:4173/'}?capture=1`);
  await page.waitForFunction(() => window.__ELEGNT_SIM_TEST__?.actionApi && window.__ELEGNT_SIM_TEST__.view.diagnostics(window.__ELEGNT_SIM_TEST__.controller.getPose()).meshes === 6);
  await page.getByLabel('模拟桥地址').fill(base);
  await page.getByLabel('模拟桥令牌').fill(token);
  await page.getByRole('button', { name: '连接模拟桥', exact: true }).click();
  await page.waitForSelector('[data-bridge-status="connected"]');
  const catalog = await submit(query('discover'));
  assert.equal(catalog.ok, true); assert.equal(catalog.result.primitives.length, 13);
  const nativePages = spawnSync(binary, ['catalog'], { encoding: 'utf8' });
  assert.equal(nativePages.status, 0);
  const pages = nativePages.stdout.trim().split(/\r?\n/).map(JSON.parse);
  function strip(v) {
    if (Array.isArray(v)) return v.map(strip);
    if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).filter(([k]) => k !== 'description').map(([k, x]) => [k, strip(x)]));
    return v;
  }
  for (const p of pages) {
    const def = p.primitives[0], actual = catalog.result.primitives.find(x => x.id === def.id);
    assert.ok(actual); assert.deepEqual(def.inputSchema, strip(actual.inputSchema));
  }
  const sequence = adapter({ protocol_version: 'lamp-action/2', request_id: randomUUID(), steps: [
    { action_id: 'set_mode', mode: 'stand' }, { action_id: 'execute_primitive', skill_id: 'A01', options: { repetitions: 1 } },
    { action_id: 'wait', duration_sec: .1 }, { action_id: 'execute_primitive', skill_id: 'B01' },
  ] });
  const accepted = await submit(sequence); assert.equal(accepted.ok, true, JSON.stringify(accepted));
  const done = await waitTask(accepted.result.task_id);
  assert.equal(done.status, 'completed'); assert.equal(done.completion_basis, 'simulation'); assert.equal(done.results.length, 4);
  assert.equal((await http('/v1/commands', sequence)).status, 200);
  assert.equal((await submit(sequence)).result.task_id, accepted.result.task_id);
  const conflict = structuredClone(sequence); conflict.args.steps[1].options.repetitions = 2;
  assert.equal((await http('/v1/commands', conflict)).status, 409);
  const invalidGroup = adapter({ protocol_version: 'lamp-action/2', request_id: randomUUID(), steps: [
    { action_id: 'execute_primitive', skill_id: 'A01' }, { action_id: 'gaze_calibrated', target_id: 'book' },
  ] });
  const rejected = await submit(invalidGroup); assert.equal(rejected.ok, false); assert.equal(rejected.error.code, 'GROUP_VALIDATION_FAILED');
  const context = { source: 'dialogue', turn_id: randomUUID(), cue: 'question' };
  const expression = () => adapter({ protocol_version: 'lamp-action/2', request_id: randomUUID(),
    steps: [{ action_id: 'execute_primitive', skill_id: 'B02' }], context });
  const expressed = await submit(expression()); assert.equal(expressed.ok, true, JSON.stringify(expressed));
  assert.equal((await waitTask(expressed.result.task_id)).status, 'completed');
  const duplicateTurn = await submit(expression()); assert.equal(duplicateTurn.ok, false); assert.equal(duplicateTurn.error.code, 'DIALOGUE_ALREADY_EXPRESSED');
  const long = await submit(adapter({ protocol_version: 'lamp-action/2', request_id: randomUUID(), steps: [{ action_id: 'wait', duration_sec: 10 }] }));
  assert.equal(long.ok, true);
  const cancelled = await submit(query('cancel', { task_id: long.result.task_id })); assert.equal(cancelled.ok, true);
  assert.equal((await waitTask(long.result.task_id)).status, 'cancelled');
  await page.getByRole('button', { name: '断开模拟桥', exact: true }).click();
  const offline = await http('/v1/commands', query('execute', { action_id: 'execute_primitive', skill_id: 'A01' }));
  assert.equal(offline.status, 503);
  assert.deepEqual(errors, []);
  console.log('PASS: production C++ catalog parity, 4-step URDF group, no replay, conflict, atomic rejection, dialogue dedup, cancellation, offline');
} finally {
  await browser.close();
  await new Promise(resolve => relay.close(resolve));
}
