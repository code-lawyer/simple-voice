import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

export const toolRoot = path.dirname(fileURLToPath(import.meta.url));
export const defaultWorkRoot = path.resolve(toolRoot, '../../build/asr-benchmark');
export const readJson = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
export const writeJson = (file, value) => fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n');
export const manifest = readJson(path.join(toolRoot, 'models.json'));
export const sourceCorpus = readJson(path.join(toolRoot, 'corpus.json'));

export function below(root, relative) {
  const base = path.resolve(root);
  const target = path.resolve(base, relative);
  const rel = path.relative(base, target);
  if (!rel || rel.startsWith('..' + path.sep) || rel === '..' || path.isAbsolute(rel)) {
    throw new Error(`Path must be inside the chosen directory: ${relative}`);
  }
  return target;
}

export async function sha256(file) {
  const hash = crypto.createHash('sha256');
  for await (const chunk of fs.createReadStream(file)) hash.update(chunk);
  return hash.digest('hex');
}

export async function verify(file, expected) {
  const actual = await sha256(file);
  if (actual !== expected) throw new Error(`SHA-256 mismatch: ${file}\nExpected ${expected}\nActual   ${actual}`);
  return actual;
}

export function readPcm16(file) {
  const wav = fs.readFileSync(file);
  if (wav.toString('ascii', 0, 4) !== 'RIFF' || wav.toString('ascii', 8, 12) !== 'WAVE') {
    throw new Error(`Expected RIFF WAV: ${file}`);
  }
  let fmt, pcm;
  for (let p = 12; p + 8 <= wav.length;) {
    const id = wav.toString('ascii', p, p + 4);
    const size = wav.readUInt32LE(p + 4);
    if (p + 8 + size > wav.length) throw new Error(`Truncated WAV: ${file}`);
    if (id === 'fmt ') fmt = wav.subarray(p + 8, p + 8 + size);
    if (id === 'data') pcm = wav.subarray(p + 8, p + 8 + size);
    p += 8 + size + (size % 2);
  }
  if (!fmt || fmt.length < 16 || !pcm || fmt.readUInt16LE(0) !== 1 ||
      fmt.readUInt16LE(2) !== 1 || fmt.readUInt16LE(14) !== 16 || pcm.length % 2) {
    throw new Error(`Expected mono PCM16 WAV: ${file}`);
  }
  return { pcm, sampleRate: fmt.readUInt32LE(4) };
}

export function writePcm16(file, pcm, sampleRate) {
  const wav = Buffer.alloc(44 + pcm.length);
  wav.write('RIFF', 0); wav.writeUInt32LE(wav.length - 8, 4);
  wav.write('WAVEfmt ', 8); wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20); wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(sampleRate, 24); wav.writeUInt32LE(sampleRate * 2, 28);
  wav.writeUInt16LE(2, 32); wav.writeUInt16LE(16, 34);
  wav.write('data', 36); wav.writeUInt32LE(pcm.length, 40); pcm.copy(wav, 44);
  fs.writeFileSync(file, wav);
}
