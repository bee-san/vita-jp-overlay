// End-to-end CLI tests over the real Hachidori Relay HTTP API.
// The relay and its socket test helpers come from pinned hachidori-anki v0.0.5.
// Only the dictionary-owning browser is replaced with a deterministic test host.
// SPDX-License-Identifier: GPL-3.0-or-later
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import { promisify } from "node:util";
import { pathToFileURL } from "node:url";
import test from "node:test";

const source = resolve(process.env.HACHIDORI_RELAY_SOURCE ?? ".refs/hachidori-anki");
const cli = resolve(process.env.VJO_CLI ?? "build/host/vjo-cli");
const { startAnkiRelayServer } = await import(pathToFileURL(resolve(source, "test/anki-relay-server.mjs")));
const { startFakeHost } = await import(pathToFileURL(resolve(source, "test/fake-host.mjs")));
const exec = promisify(execFile);

async function run(args, env = {}) {
  try {
    const result = await exec(cli, args, { timeout: 15000,
      env: { ...process.env, VJO_HACHIDORI_HOST: "", VJO_JPDB_KEY: "", VJO_JITEN_KEY: "", ...env } });
    return { code: 0, ...result };
  } catch (error) {
    if (typeof error.code !== "number") throw error;
    return { code: error.code, stdout: error.stdout, stderr: error.stderr };
  }
}

async function relay(t) {
  const result = await startAnkiRelayServer({ apiPort: 0 });
  t.after(() => result.close());
  assert.ok(result.apiPort > 0, result.apiError ?? "relay HTTP listener did not start");
  return result;
}

function entry(expression, reading, originalText, glosses, rank = 99999) {
  return {
    type: "term", isPrimary: true,
    headwords: [{ term: expression, reading, sources: [{ originalText, isPrimary: true }] }],
    definitions: [{ dictionary: "Test dictionary", entries: glosses }],
    frequencies: [{ frequency: rank }],
  };
}

const words = [
  ["見た", "見る", "みる", ["to see", { type: "structured-content", content: { tag: "span", style: { color: "red" }, content: "to look" } }], 123],
  ["猫", "猫", "ねこ", ["cat"], 456],
  ["犬", "犬", "いぬ", ["dog"], 789],
];

function respond(message) {
  assert.equal(message.type, "hd_api_term_entries");
  assert.equal(message.target, "hoshidicts-offscreen");
  assert.equal(message.terms.length, 1);
  return { results: message.terms.map((text, index) => {
    const match = words.find(([original]) => text.startsWith(original));
    return { index, originalTextLength: match?.[0].length ?? 0,
      dictionaryEntries: match ? [entry(match[1], match[2], match[0], match[3], match[4])] : [] };
  }) };
}

async function hosting(t, options = {}) {
  const server = await relay(t);
  const host = await startFakeHost(t, server.port, { respond, ...options });
  return { ...server, host };
}

function args(apiPort, text) {
  return ["--dict", "hachidori", "--hachidori", `127.0.0.1:${apiPort}`, "--text", text, "--nav", "--stats"];
}

// RED on upstream: the CLI has no relay backend or --hachidori switch.
test("the fork defaults to Hachidori and explains a missing relay address", async () => {
  const result = await run(["--text", "猫"]);
  assert.equal(result.code, 1);
  assert.match(result.stdout, /hachidori_host/iu);
  assert.doesNotMatch(result.stdout, /API key is not set/iu);
});

test("real relay lookups display words, conjugations, structured definitions and ranks without a key", async (t) => {
  const { apiPort, host } = await hosting(t);
  const result = await run(args(apiPort, "猫を見た。犬も見た。"));
  assert.equal(result.code, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /猫 \(ねこ\) 456/u);
  assert.match(result.stdout, /見る \(みる\) 123/u);
  assert.match(result.stdout, /犬 \(いぬ\) 789/u);
  assert.match(result.stdout, /to see/u);
  assert.match(result.stdout, /to look/u);
  assert.doesNotMatch(result.stdout, /structured-content|color|span|\[error\]/u);
  assert.ok(host.requests.length >= 3);
  assert.ok(host.requests.every(({ message }) => message.type === "hd_api_term_entries"));
});

test("relay settings work from config.ini without CLI dictionary or API-key overrides", async (t) => {
  const { apiPort } = await hosting(t);
  const directory = await mkdtemp(resolve(tmpdir(), "vjo-relay-config-"));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const config = resolve(directory, "config.ini");
  await writeFile(config, `dictionary = hachidori\nhachidori_host = 127.0.0.1:${apiPort}\n`);
  const result = await run(["--config", config, "--text", "猫"]);
  assert.equal(result.code, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /cat/u);
});

test("explicit legacy dictionary selections still use their own key requirements", async () => {
  for (const dictionary of ["jpdb", "jiten"]) {
    const result = await run(["--dict", dictionary, "--text", "猫"]);
    assert.equal(result.code, 1);
    assert.match(result.stdout, /API key is not set/u);
    assert.doesNotMatch(result.stdout, /hachidori_host/iu);
  }
});

test("supplementary Unicode and OCR line breaks keep correct word highlights", async (t) => {
  const { apiPort } = await hosting(t);
  const result = await run(args(apiPort, "😀猫を\n見た。"));
  assert.equal(result.code, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /1: 😀【猫】を\n見た。/u);
  assert.match(result.stdout, /2: 😀猫を\n【見た】。/u);
});

test("repeated words are deduplicated without losing the first token's position", async (t) => {
  const { apiPort } = await hosting(t);
  const result = await run(args(apiPort, "猫、猫。"));
  assert.equal(result.code, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /1: 【猫】、猫。/u);
  assert.doesNotMatch(result.stdout, /2: /u);
});

test("a sentence with no dictionary matches is not an error", async (t) => {
  const { apiPort } = await hosting(t);
  const result = await run(args(apiPort, "未登録語。"));
  assert.equal(result.code, 0, result.stdout + result.stderr);
  assert.doesNotMatch(result.stdout, /\[error\]|1: /u);
});

test("a real relay without a sharing browser reports HTTP 503 rather than cloud fallback", async (t) => {
  const { apiPort } = await relay(t);
  const result = await run(args(apiPort, "猫"));
  assert.equal(result.code, 1);
  assert.match(result.stdout, /503/u);
  assert.match(result.stdout, /No sharing Hachidori/iu);
  assert.doesNotMatch(result.stdout, /jpdb|jiten|API key/iu);
});

test("an incompatible sharing browser reports the relay's update advice", async (t) => {
  const { apiPort } = await hosting(t, { capabilities: [] });
  const result = await run(args(apiPort, "猫"));
  assert.equal(result.code, 1);
  assert.match(result.stdout, /501/u);
  assert.match(result.stdout, /update Hachidori/iu);
});

test("a dictionary host failure preserves the relay's error message", async (t) => {
  const { apiPort } = await hosting(t, { respond: () => ({ error: "test dictionary unavailable" }) });
  const result = await run(args(apiPort, "猫"));
  assert.equal(result.code, 1);
  assert.match(result.stdout, /500/u);
  assert.match(result.stdout, /test dictionary unavailable/u);
});
