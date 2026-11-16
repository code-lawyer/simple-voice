// Score the production worker's smoke logs against the existing, labelled
// diagnostic corpus. Usage: node tools/asr-benchmark/score-runtime.mjs RESULTS
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
const root = path.resolve(process.argv[2] ?? 'build-review/public-sprint');
const corpus = JSON.parse(fs.readFileSync(new URL('./corpus.json', import.meta.url), 'utf8')).cases;
const input = fs.readFileSync(path.join(root, 'runtime-corpus.tsv'), 'utf8').trim().split(/\r?\n/).map(line => line.split('\t'));
const hashes = Object.fromEntries(input.map(([id, file]) => [id, crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')]));
for (const c of corpus) if (hashes[c.id] && c.recordedSha256 !== hashes[c.id]) throw new Error(`Corpus drift: ${c.id}`);
const normal = text => text.normalize('NFKC').toLowerCase().replace(/[^\p{L}\p{N}]+/gu, ' ').trim();
const words = text => normal(text).split(/\s+/).filter(Boolean);
function distance(a, b) {
  let previous = Array.from({ length: b.length + 1 }, (_, i) => i);
  for (let i = 1; i <= a.length; ++i) {
    const current = [i];
    for (let j = 1; j <= b.length; ++j) current[j] = Math.min(current[j - 1] + 1, previous[j] + 1, previous[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    previous = current;
  }
  return previous[b.length];
}
function score(reference, text) {
  const a = words(reference), b = words(text);
  return { errors: distance(a, b), referenceWords: a.length };
}
const stressReference = Array(18).fill('Please open the project and save the file.').join(' ');
const result = { note: 'Diagnostic observations only. TTS and upstream ASR example text are not independent natural-speech acceptance. PASS in logs means runtime/text-preservation invariants, not transcription accuracy.', hashes, models: {} };
for (const model of ['qwen3', 'sensevoice', 'whisper']) {
  const log = fs.readFileSync(path.join(root, `corpus-${model}.log`), 'utf8');
  const cases = [];
  for (const match of log.matchAll(/^(PASS|FAIL) (\S+) duration=([\d.]+)s final_ms=(\d+)\r?\nraw=([^\r\n]*)\r?\nfinal=([^\r\n]*)\r?\nwarning=([^\r\n]*)/gm)) {
    const c = corpus.find(c => c.id === match[2]);
    const row = { id: match[2], invariants: match[1], durationSeconds: +match[3], finalWaitMs: +match[4], text: match[6], warning: match[7], referenceKind: c?.reference_kind ?? 'synthetic_repeated_tts' };
    if (c?.reference != null && c.language === 'en') row.wordScore = score(c.reference, row.text);
    if (row.id === 'stress_60s') row.wordScore = score(stressReference, row.text);
    cases.push(row);
  }
  if (cases.length !== input.length) throw new Error(`Missing observations: ${model}`);
  const tts = cases.filter(c => c.referenceKind === 'known_tts_prompt');
  const errors = tts.reduce((n, c) => n + c.wordScore.errors, 0), referenceWords = tts.reduce((n, c) => n + c.wordScore.referenceWords, 0);
  const stress = cases.find(c => c.id === 'stress_60s');
  result.models[model] = { cases, ttsWer: referenceWords ? errors / referenceWords : null, stressWer: stress.wordScore.errors / stress.wordScore.referenceWords };
  console.log(`${model}: ${cases.length} observations; synthetic short English WER ${(errors / referenceWords * 100).toFixed(2)}%; repeated 60 s English WER ${(result.models[model].stressWer * 100).toFixed(2)}%; final wait ${stress.finalWaitMs} ms`);
}
fs.writeFileSync(path.join(root, 'runtime-quality.json'), JSON.stringify(result, null, 2) + '\n');
