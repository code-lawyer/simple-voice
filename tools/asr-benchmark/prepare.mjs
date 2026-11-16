// Public resources only. Downloads are opt-in; existing resources are read and hashed.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { spawnSync } from 'node:child_process';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { randomUUID } from 'node:crypto';
import { toolRoot, defaultWorkRoot, manifest, sourceCorpus, below, sha256, verify, writeJson, writePcm16 } from './common.mjs';

const { values } = parseArgs({ options: {
  'work-root': { type: 'string', default: defaultWorkRoot },
  'reuse-root': { type: 'string', multiple: true, default: [] },
  configurations: { type: 'string', default: manifest.defaults.configurations.join(',') },
  download: { type: 'boolean', default: false },
  'generate-tts': { type: 'boolean', default: false },
  voice: { type: 'string', default: 'Microsoft Zira Desktop' },
  help: { type: 'boolean', default: false }
} });
if (values.help) {
  console.log('node prepare.mjs [--work-root DIR] [--reuse-root DIR ...] [--download] [--generate-tts] [--voice NAME] [--configurations qwen,zip_online]');
  process.exit(0);
}
const root = path.resolve(values['work-root']);
const reuseRoots = values['reuse-root'].map(p => path.resolve(p));
const requested = values.configurations.split(',').filter(Boolean);
const configurations = requested.map(kind => {
  const c = manifest.configurations.find(c => c.kind === kind);
  if (!c) throw new Error(`Unknown configuration: ${kind}`);
  return c;
});
fs.mkdirSync(root, { recursive: true });
for (const directory of ['downloads', 'artifacts', 'audio']) fs.mkdirSync(below(root, directory), { recursive: true });

function run(command, args) {
  const r = spawnSync(command, args, { encoding: 'utf8', windowsHide: true, maxBuffer: 8 * 1024 * 1024 });
  if (r.error) throw r.error;
  if (r.status !== 0) throw new Error(`${command} failed (${r.status}): ${r.stderr || r.stdout}`);
  return r.stdout;
}

async function download(url, target, expected) {
  if (fs.existsSync(target)) return verify(target, expected);
  if (!values.download) throw new Error(`Missing ${target}. Add --download to obtain the fixed public resource: ${url}`);
  if (!url.startsWith('https://')) throw new Error('Only HTTPS downloads are supported');
  console.log(`Downloading ${path.basename(target)}`);
  const temporary = target + '.' + randomUUID() + '.part';
  try {
    const response = await fetch(url);
    if (!response.ok || !response.body) throw new Error(`Download failed: HTTP ${response.status} ${url}`);
    await pipeline(Readable.fromWeb(response.body), fs.createWriteStream(temporary, { flags: 'wx' }));
    await verify(temporary, expected);
    fs.renameSync(temporary, target);
  } finally {
    if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
  }
}

function directories(artifact) {
  const candidates = [below(root, 'artifacts/' + artifact.folder)];
  for (const base of reuseRoots) {
    candidates.push(base, below(base, artifact.folder), below(base, 'models/' + artifact.folder));
    for (const alias of artifact.reusePaths || []) candidates.push(below(base, alias));
  }
  return [...new Set(candidates)];
}

const extracted = new Map();
async function extract(artifact) {
  if (extracted.has(artifact.id)) return extracted.get(artifact.id);
  if (!artifact.archive) throw new Error(`No archive for ${artifact.id}`);
  const name = new URL(artifact.archive.url).pathname.split('/').at(-1);
  const candidates = [below(root, 'downloads/' + name), ...reuseRoots.flatMap(r => [below(r, 'downloads/' + name), below(r, name)])];
  let archive = candidates.find(f => fs.existsSync(f));
  if (archive) await verify(archive, artifact.archive.sha256);
  else {
    archive = candidates[0];
    await download(artifact.archive.url, archive, artifact.archive.sha256);
  }
  const entries = run('tar', ['-tf', archive]).split(/\r?\n/).filter(Boolean);
  for (const entry of entries) {
    const normalized = entry.replaceAll('\\', '/').replace(/^\.\//, '');
    if (normalized !== artifact.folder && !normalized.startsWith(artifact.folder + '/')) throw new Error(`Unexpected archive root: ${entry}`);
    if (normalized.split('/').includes('..') || normalized.includes(':')) throw new Error(`Unsafe archive path: ${entry}`);
  }
  const destination = below(root, 'artifacts');
  run('tar', ['-xf', archive, '-C', destination]);
  const dir = below(destination, artifact.folder);
  extracted.set(artifact.id, dir);
  return dir;
}

const resolved = new Map();
async function artifactRoot(id) {
  if (resolved.has(id)) return resolved.get(id);
  const artifact = manifest.artifacts.find(a => a.id === id);
  if (!artifact) throw new Error(`Unknown artifact: ${id}`);
  let dir = directories(artifact).find(candidate => artifact.files.every(f => fs.existsSync(below(candidate, f.file))));
  if (!dir) {
    if (artifact.archive) dir = await extract(artifact);
    else {
      dir = below(root, 'artifacts/' + artifact.folder);
      fs.mkdirSync(dir, { recursive: true });
      for (const f of artifact.files) {
        const target = below(dir, f.file);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        await download(f.url, target, f.sha256);
      }
    }
  }
  for (const f of artifact.files) await verify(below(dir, f.file), f.sha256);
  resolved.set(id, dir);
  console.log(`Verified ${id}`);
  return dir;
}

await artifactRoot('runtime');
for (const c of configurations) await artifactRoot(c.artifact);
const cases = [];
for (const c of sourceCorpus.cases) {
  const target = below(root, c.file);
  if (!fs.existsSync(target)) {
    const copies = reuseRoots.flatMap(r => [below(r, c.id + '.wav'), below(r, 'audio/' + c.id + '.wav'), below(r, 'corpus-snapshot/' + c.id + '.wav')]);
    let original = copies.find(f => fs.existsSync(f));
    if (original) {
      await verify(original, c.recordedSha256);
      fs.copyFileSync(original, target);
    } else if (c.acquire.kind === 'archive') {
      const artifact = manifest.artifacts.find(a => a.id === c.acquire.artifact);
      original = directories(artifact).map(d => below(d, c.acquire.file)).find(f => fs.existsSync(f));
      if (!original) original = below(await extract(artifact), c.acquire.file);
      await verify(original, c.recordedSha256);
      fs.copyFileSync(original, target);
    } else if (c.acquire.kind === 'download') {
      await download(c.acquire.url, target, c.recordedSha256);
    } else if (c.acquire.kind === 'silence') {
      writePcm16(target, Buffer.alloc(c.acquire.sampleRate * c.acquire.seconds * 2), c.acquire.sampleRate);
    } else if (c.acquire.kind === 'windows_tts') {
      if (!values['generate-tts']) throw new Error(`Missing ${c.id}. Use --generate-tts or --reuse-root containing the recorded diagnostic WAVs.`);
      if (process.platform !== 'win32') throw new Error('Windows TTS generation requires Windows; reuse already-generated diagnostic WAVs instead.');
      run('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.join(toolRoot, 'generate-tts.ps1'), '-WorkRoot', root, '-Voice', values.voice]);
    }
  }
  const actualSha256 = await sha256(target);
  const matchesRecordedAudio = actualSha256 === c.recordedSha256;
  if (!matchesRecordedAudio && c.acquire.kind !== 'windows_tts') await verify(target, c.recordedSha256);
  if (!matchesRecordedAudio) console.warn(`${c.id}: TTS audio differs from the recorded Windows voice; metrics are a new diagnostic run.`);
  cases.push({ ...c, path: target, actualSha256, matchesRecordedAudio });
}
writeJson(below(root, 'corpus.json'), { ...sourceCorpus, cases });
fs.writeFileSync(below(root, 'corpus.tsv'), cases.map(c => c.id + '\t' + c.path).join('\n') + '\n');
writeJson(below(root, 'layout.json'), { schema: 1, artifacts: Object.fromEntries(resolved), requestedConfigurations: requested, corpus: 'corpus.tsv', preparedAt: new Date().toISOString() });
console.log(`Ready: ${configurations.length} configurations, ${cases.length} diagnostic files. Work root: ${root}`);
