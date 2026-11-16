# Model-free long-entry gate

On top of the pinned Wanxiang top-200000 core selection, retain all one/two
character entries. Retain longer entries only when their upstream weight is
at least 5000, or when the identical text and code already occur in the
project's original pinyin_simp dictionary, or in the documented computing terms
catalogue. Preserve all retained upstream weights, without scaling or mixing
old dictionary weights. This is a conservative data filter, not a language model.
The old dictionary is used as an allow-list only; no old entry content is copied.
Removed 134206 long entries. No evaluation sentence was added to the data.
