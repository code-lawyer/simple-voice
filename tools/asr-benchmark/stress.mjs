import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { defaultWorkRoot, below, readJson, writeJson, readPcm16, writePcm16, sha256 } from './common.mjs';

const { values } = parseArgs({ options: { 'work-root': { type: 'string', default: defaultWorkRoot } } });
const root = path.resolve(values['work-root']);
const corpus = readJson(below(root, 'corpus.json'));
const sentence = corpus.cases.find(c => c.id === 'tts_english_sentence');
const source = readPcm16(sentence.path);
if (source.sampleRate !== 16000) throw new Error('Stress source must be 16 kHz');
const sr = source.sampleRate;
const pcm = Buffer.alloc(sr * 2 * 60);
const pause = sr * 2 / 4;
let cursor = 0, repeats = 0;
while (cursor + source.pcm.length + pause <= pcm.length) {
  source.pcm.copy(pcm, cursor);
  cursor += source.pcm.length + pause;
  ++repeats;
}
if (!repeats) throw new Error('TTS prompt is too long for the 60 second stress case');
const whole = below(root, 'audio/stress_60s.wav');
writePcm16(whole, pcm, sr);
const suites = { stress_whole: [{ id: 'stress_60s', path: whole, start_sample: 0, end_sample: pcm.length / 2 }] };

function energy(start, end) {
  let sum = 0;
  for (let i = start; i < end; ++i) { const x = pcm.readInt16LE(i * 2) / 32768; sum += x * x; }
  return sum / (end - start);
}

for (const [suite, minimum, maximum] of [['stress20', 15, 20], ['stress15', 10, 15]]) {
  const parts = [];
  let start = 0;
  while (start < pcm.length / 2) {
    let end = pcm.length / 2, cut = null;
    if (end - start > maximum * sr) {
      let best = Infinity;
      for (let center = start + minimum * sr; center <= start + maximum * sr; center += sr / 100) {
        const value = energy(center - sr / 100, center + sr / 100);
        if (value <= best) { best = value; end = center; }
      }
      cut = { search_start_s: start / sr + minimum, search_end_s: start / sr + maximum, rms: Math.sqrt(best), window_ms: 20, step_ms: 10, tie_break: 'latest minimum', quiet_under_fixed_threshold: best <= 0.0001 };
    }
    const id = suite + '_part_' + (parts.length + 1);
    const file = below(root, 'audio/' + id + '.wav');
    writePcm16(file, pcm.subarray(start * 2, end * 2), sr);
    parts.push({ id, path: file, start_sample: start, end_sample: end, duration_seconds: (end - start) / sr, cut });
    start = end;
  }
  const reconstructed = Buffer.concat(parts.map(p => readPcm16(p.path).pcm));
  if (!reconstructed.equals(pcm)) throw new Error(`Lost or duplicated PCM in ${suite}`);
  suites[suite] = parts;
}
for (const [suite, parts] of Object.entries(suites)) {
  fs.writeFileSync(below(root, suite + '.tsv'), parts.map(p => p.id + '\t' + p.path).join('\n') + '\n');
}
writeJson(below(root, 'stress.json'), {
  purpose: 'Synthetic repeated-prompt length and resource stress; not an independent accuracy corpus',
  sourceSha256: await sha256(sentence.path), audioSha256: await sha256(whole),
  sampleRate: sr, durationSeconds: 60, repeats, sentence: sentence.reference,
  reference: Array(repeats).fill(sentence.reference).join(' '),
  pcmCoverageVerified: true, textDeduplication: false, suites
});
console.log(`Prepared 60 second stress with ${repeats} prompt repetitions; both partitions preserve all PCM bytes.`);
