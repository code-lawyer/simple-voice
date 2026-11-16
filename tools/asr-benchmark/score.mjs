import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { defaultWorkRoot, below, readJson, writeJson, verify } from './common.mjs';

const { values } = parseArgs({ options: {
  'work-root': { type: 'string', default: defaultWorkRoot },
  'results-root': { type: 'string' }
} });
if (!values['results-root']) throw new Error('--results-root is required');
const root = path.resolve(values['work-root']);
const resultsRoot = path.resolve(values['results-root']);
const metadata = readJson(below(resultsRoot, 'run.json'));
if (metadata.corpusManifestSha256) await verify(below(root, 'corpus.json'), metadata.corpusManifestSha256);
if (metadata.stressManifestSha256) await verify(below(root, 'stress.json'), metadata.stressManifestSha256);
const corpus = readJson(below(root, 'corpus.json')).cases;
const stress = fs.existsSync(below(root, 'stress.json')) ? readJson(below(root, 'stress.json')) : null;
const records = fs.readdirSync(resultsRoot).filter(f => f.endsWith('.jsonl')).flatMap(file =>
  fs.readFileSync(below(resultsRoot, file), 'utf8').trim().split(/\r?\n/).filter(Boolean).map(line => ({ ...JSON.parse(line), file })));
const median = values => { const sorted = [...values].sort((a, b) => a - b); const half = Math.floor(sorted.length / 2); return sorted.length % 2 ? sorted[half] : (sorted[half - 1] + sorted[half]) / 2; };
const chars = text => [...text.normalize('NFKC').toLowerCase().replace(/[^\p{L}\p{N}]/gu, '')];
const words = text => text.normalize('NFKC').toLowerCase().match(/[\p{L}\p{N}]+/gu) || [];
function distance(a, b) {
  let previous = Array.from({ length: b.length + 1 }, (_, i) => i);
  for (let i = 1; i <= a.length; ++i) {
    const next = [i];
    for (let j = 1; j <= b.length; ++j) next[j] = Math.min(next[j - 1] + 1, previous[j] + 1, previous[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    previous = next;
  }
  return previous[b.length];
}
function score(reference, hypothesis, split) {
  const ref = split(reference), hyp = split(hypothesis), errors = distance(ref, hyp);
  return { errors, reference_units: ref.length, rate: ref.length ? errors / ref.length : null };
}
const normalizeDigits = text => text.replace(/幺/g, '一').replace(/[0-9]/g, n => '零一二三四五六七八九'[Number(n)]);
const summary = { schema: 1, complete: true, method: { threads: metadata.threads, provider: 'cpu', normalization: 'NFKC, lowercase; punctuation separates words; CER strips punctuation and whitespace; no deduplication', privateMemory: 'Maximum sample after loading or a clip, not continuously measured peak', accuracyScope: 'TTS prompt WER, isolated digit labels, and publisher number transcript are separate; upstream ASR demonstrations are not gold references' }, groups: {} };

for (const record of records) {
  const plan = metadata.plan.find(p => p.kind === record.kind && p.suite === record.suite);
  if (!plan || !plan.runs.includes(record.run) ||
      (record.type !== 'load' && record.type !== 'sample') ||
      (record.type === 'sample' && !plan.ids.includes(record.id))) {
    throw new Error(`Result is not in this run plan: ${record.file}`);
  }
}

for (const plan of metadata.plan) {
  const groupRows = records.filter(r => r.kind === plan.kind && r.suite === plan.suite);
  const expectedRows = plan.runs.length * (1 + plan.ids.length);
  if (groupRows.length !== expectedRows) throw new Error(`Incomplete or duplicate rows for ${plan.suite}/${plan.kind}`);
  for (const run of plan.runs) {
    const rows = groupRows.filter(r => r.run === run);
    if (rows.filter(r => r.type === 'load').length !== 1 || rows.some(r => r.threads !== metadata.threads)) throw new Error('Invalid load/threads metadata');
    for (const id of plan.ids) if (rows.filter(r => r.type === 'sample' && r.id === id).length !== 1) throw new Error(`Missing/duplicate ${id} run ${run}`);
  }
  const loads = groupRows.filter(r => r.type === 'load');
  const group = {
    run_count: plan.runs.length,
    load_ms: loads.map(r => r.load_ms), load_ms_median: median(loads.map(r => r.load_ms)),
    loaded_working_mib_median: median(loads.map(r => r.working_set_bytes / 1048576)),
    peak_working_mib: Math.max(...groupRows.map(r => r.peak_working_set_bytes / 1048576)),
    max_sampled_private_mib: Math.max(...groupRows.map(r => r.private_bytes / 1048576))
  };
  if (plan.suite === 'main') {
    group.cases = corpus.map(c => {
      const rows = groupRows.filter(r => r.type === 'sample' && r.id === c.id);
      return {
        id: c.id, reference_kind: c.reference_kind, matches_recorded_audio: c.matchesRecordedAudio,
        stable: rows.every(r => r.text === rows[0].text),
        elapsed_ms_median: median(rows.map(r => r.elapsed_ms)),
        runs: rows.map(r => {
          const result = { ...r };
          if (c.reference !== null && c.language !== 'silence') {
            result.cer = score(c.reference, r.text, chars);
            if (c.language === 'en') result.wer = score(c.reference, r.text, words);
            if (c.id === 'qwen_zh_numbers') result.digit_normalized_cer = score(normalizeDigits(c.reference), normalizeDigits(r.text), chars);
          }
          if (c.reference_kind === 'dataset_label') {
            const normalized = words(r.text).join(' ');
            result.english_or_arabic_digit_preserved = normalized === String(c.label) || normalized === c.reference;
            result.digit_semantic_correct = result.english_or_arabic_digit_preserved || normalized === '零一二三四五六七八九'[c.label];
          }
          if (c.critical) {
            let text = r.text.toLowerCase();
            result.critical_hits = c.critical.map(term => { const i = text.indexOf(term.toLowerCase()); if (i < 0) return false; text = text.slice(0, i) + ' '.repeat(term.length) + text.slice(i + term.length); return true; });
          }
          return result;
        })
      };
    });
    group.tts_wer_by_run = plan.runs.map(run => {
      const scores = group.cases.filter(c => c.reference_kind === 'known_tts_prompt').map(c => c.runs.find(r => r.run === run).wer);
      const errors = scores.reduce((sum, s) => sum + s.errors, 0), referenceWords = scores.reduce((sum, s) => sum + s.reference_units, 0);
      return { run, errors, reference_words: referenceWords, rate: errors / referenceWords };
    });
    group.digits_by_run = plan.runs.map(run => {
      const rows = group.cases.filter(c => c.reference_kind === 'dataset_label').map(c => c.runs.find(r => r.run === run));
      return { run, count: rows.length, english_or_arabic_preserved: rows.filter(r => r.english_or_arabic_digit_preserved).length, semantic_correct: rows.filter(r => r.digit_semantic_correct).length };
    });
  } else {
    if (!stress || !stress.suites[plan.suite]) throw new Error(`Unknown stress suite ${plan.suite}`);
    group.expected_repetitions = stress.repeats;
    group.pcm_coverage_verified = stress.pcmCoverageVerified;
    group.runs = plan.runs.map(run => {
      const rows = plan.ids.map(id => groupRows.find(r => r.run === run && r.type === 'sample' && r.id === id));
      const text = rows.map(r => r.text).join(' ');
      return { run, text, elapsed_ms: rows.reduce((sum, r) => sum + r.elapsed_ms, 0), wer: score(stress.reference, text, words), parts: rows };
    });
  }
  summary.groups[`${plan.suite}/${plan.kind}`] = group;
}
writeJson(below(resultsRoot, 'summary.json'), summary);
for (const [key, group] of Object.entries(summary.groups)) {
  const scores = group.tts_wer_by_run
    ? 'TTS word errors ' + group.tts_wer_by_run.map(r => `${r.errors}/${r.reference_words}`).join(', ')
    : 'stress word errors ' + group.runs.map(r => `${r.wer.errors}/${r.wer.reference_units}`).join(', ');
  console.log(`${key}: ${group.run_count} runs; load ${group.load_ms_median.toFixed(1)} ms; peak working ${group.peak_working_mib.toFixed(1)} MiB; ${scores}`);
}
