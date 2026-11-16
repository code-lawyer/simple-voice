# Data sources and modifications

Chinese data: Copyright amzxyz and rime-wanxiang-data contributors.
https://github.com/amzxyz/rime-wanxiang-data/tree/7de2f3d0b4677d1e058eecdd48ac412d0314edf9
Files: dicts/zi.dict.yaml, dicts/jichu.dict.yaml, dicts/duoyin.dict.yaml.
License: CC BY 4.0; full license and upstream scope statement accompany this data.
Modified by Simple Voice: tone marks removed, ü mapped to v, identical (text,code)
rows merged by maximum source weight; top 200000 positive-frequency 2-8-character
entries retained, a documented computing vocabulary supplement retained, and
pronunciation variants retained for selected words. Original weights are preserved.
No endorsement by upstream is implied. No Wanxiang program code is redistributed.

English data: English Speller Database (ESDB), Copyright 2000-2026 Kevin Atkinson.
https://github.com/en-wl/wordlist/tree/1e5b7d3a72f47a71da5d28686c1dd4b397178485
The complete upstream Copyright notice is included as esdb.Copyright.
Generated from the pinned source database: US/A variants <=1, default category,
no abbreviations, single words through size 50, open compounds through size 60.
ASCII spellings only, length 3-40, possessives omitted. Codes remove internal
spaces, apostrophes and hyphens. Weights express source size class, not observed
usage counts. Upstream build tools are used only for local reproducibility.

Canonical case-sensitive English aliases are retained alongside lowercase codes.
The explicit product/development and mixed-language supplements are authored for
Simple Voice and listed verbatim in prepare_data.py; they are not user learning
or claimed as ESDB entries. English supplements share the English translator;
mixed-language entries use a separate translator. No Rime-Ice files are used.

Final model-free core selection applies CORE-POLICY.md after the initial 200000
candidate selection: 67163 word/code rows are retained. The original pinyin_simp
word/code membership list is used only to preserve established longer words;
old frequencies are not merged into this data. That allow-list comes from
rime/rime-pinyin-simp at 0c6861ef7420ee780270ca6d993d18d4101049d0, derived from
Android PinyinIME. Its Apache-2.0 license, AUTHORS and source record are included.
https://github.com/rime/rime-pinyin-simp/tree/0c6861ef7420ee780270ca6d993d18d4101049d0

Schema configuration and source conversion scripts are authored for Simple Voice.
Generated assets contain no Lua, no Grammar model and no executable upstream
Wanxiang tool. Copy the attribution/license files with any redistributed data.
