"""Compare stable reference positions in two recordings of streaming text changes."""
import argparse
import json
import re
import statistics
import unicodedata
from difflib import SequenceMatcher
from pathlib import Path


def units(text):
    text = unicodedata.normalize("NFKC", text).casefold()
    return re.findall(r"[a-z]+(?:'[a-z]+)?|[0-9]+|[^\W\d_a-z]", text)


def reference_times(changes, reference):
    if not changes:
        return {}, 0, 0
    final = units(changes[-1][1])
    first = [None] * len(final)
    stable = len(final)
    rewrites = 0
    previous = ""
    for _, text in changes:
        current = unicodedata.normalize("NFKC", text).strip().casefold()
        rewrites += not current.startswith(previous)
        previous = current
    # A position counts only if its entire hypothesis prefix survives every
    # later update. Match contiguous reference spans to exclude isolated hits.
    for time_ms, text in reversed(changes):
        current = units(text)
        shared = next((i for i, (a, b) in enumerate(zip(current, final)) if a != b),
                      min(len(current), len(final)))
        stable = min(stable, shared)
        for i in range(stable):
            first[i] = time_ms
    times = {}
    for a, b, size in SequenceMatcher(None, reference, final, autojunk=False).get_matching_blocks():
        if size >= 2:
            times.update((a + i, first[b + i]) for i in range(size))
    return times, rewrites, len(final)


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "corpus", "fleurs"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    references = {row["id"]: row["reference"] for row in load(args.corpus)["cases"]}
    references.update({Path(row["sourceFile"]).stem: row["reference"] for row in load(args.fleurs)})
    before, after = ({r["name"]: r["changes"] for r in load(path)} for path in (args.baseline, args.candidate))
    if before.keys() != after.keys():
        raise ValueError("Trace sets differ")
    differences = []
    totals = [0, 0]
    rewrites = [0, 0]
    per_clip = {}
    for name in before:
        ref = units(references.get(name) or "")
        measurements = [reference_times(trace[name], ref) for trace in (before, after)]
        old, new = (entry[0] for entry in measurements)
        for i, (times, edits, _) in enumerate(measurements):
            totals[i] += len(times)
            rewrites[i] += edits
        delta = [old[i] - new[i] for i in old.keys() & new.keys()
                 if i >= 2 and min(old[i], new[i]) >= 2000]
        if delta:
            differences.extend(delta)
            per_clip[name] = statistics.median(delta)
    print(json.dumps(dict(recordings=len(before), scored_clips=len(per_clip),
                          stable_reference_units=totals, revised_updates=rewrites,
                          matched_mid_sentence_units=len(differences),
                          median_advance_ms=statistics.median(differences) if differences else None,
                          median_per_clip_ms=statistics.median(per_clip.values()) if per_clip else None,
                          earlier=sum(d > 0 for d in differences), later=sum(d < 0 for d in differences),
                          by_clip_ms=per_clip), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
