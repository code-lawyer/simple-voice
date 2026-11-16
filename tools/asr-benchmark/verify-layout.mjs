import path from 'node:path';
import { parseArgs } from 'node:util';
import { defaultWorkRoot, manifest, below, readJson, verify } from './common.mjs';

const { values } = parseArgs({ options: { 'work-root': { type: 'string', default: defaultWorkRoot } } });
const root = path.resolve(values['work-root']);
const layout = readJson(below(root, 'layout.json'));
const corpus = readJson(below(root, 'corpus.json'));
for (const [id, directory] of Object.entries(layout.artifacts)) {
  const artifact = manifest.artifacts.find(a => a.id === id);
  if (!artifact) throw new Error(`Unknown prepared artifact: ${id}`);
  for (const file of artifact.files) await verify(below(directory, file.file), file.sha256);
}
for (const sample of corpus.cases) await verify(sample.path, sample.actualSha256);
console.log(`Verified prepared runtime/models and ${corpus.cases.length} actual audio hashes before timing.`);
