# Third-party notices

Simple Voice's first-party code is licensed under Apache-2.0. Third-party code,
dictionaries, runtime libraries and models retain their own licenses.

## Included in the source or Windows application ZIP

| Component | Version / source | License text |
| --- | --- | --- |
| librime | [1.17.0](https://github.com/rime/librime/tree/1.17.0), statically linked | [BSD-3-Clause](licenses/librime.txt) |
| Boost | [1.86.0](https://github.com/boostorg/boost/tree/boost-1.86.0), headers | [BSL-1.0](licenses/boost.txt) |
| yaml-cpp | [0.8.0](https://github.com/jbeder/yaml-cpp/tree/0.8.0) | [MIT](licenses/yaml-cpp.txt) |
| LevelDB | [1.23](https://github.com/google/leveldb/tree/1.23) | [BSD-3-Clause](licenses/leveldb.txt) |
| OpenCC | [1.1.9](https://github.com/BYVoid/OpenCC/tree/ver.1.1.9), library only | [Apache-2.0](licenses/opencc.txt) |
| marisa-trie | [0.2.6](https://github.com/s-yata/marisa-trie/tree/v0.2.6) | [BSD-2-Clause option](licenses/marisa.txt) |
| cppjieba | [cbdf949](https://github.com/yanyiwu/cppjieba/tree/cbdf949eadb8887f060473b5dc6d3194655c8bef), code and dictionaries | [MIT](licenses/cppjieba.txt) |
| pinyin-data | [fe29ad3](https://github.com/mozillazg/pinyin-data/tree/fe29ad32bdf352b31a02b9936e537f575432e9a6), pronunciation table | [MIT](licenses/pinyin-data.txt) |
| rime-pinyin-simp | [0c6861e](https://github.com/rime/rime-pinyin-simp/tree/0c6861ef7420ee780270ca6d993d18d4101049d0), dictionary | [Apache-2.0](licenses/rime-pinyin-simp.txt), [authors](rime-data/pinyin_simp.AUTHORS) |
| rime-wanxiang-data | [7de2f3d](https://github.com/amzxyz/rime-wanxiang-data/tree/7de2f3d0b4677d1e058eecdd48ac412d0314edf9), selected character/word pronunciation data only | [CC BY 4.0](rime-data/wanxiang.LICENSE), [scope](rime-data/wanxiang.LICENSE-SCOPE.md), [attribution and modifications](rime-data/wanxiang.NOTICE.md); no Wanxiang tool code |
| English Speller Database | [1e5b7d3](https://github.com/en-wl/wordlist/tree/1e5b7d3a72f47a71da5d28686c1dd4b397178485), selected US words and open compounds | [Complete upstream Copyright](rime-data/esdb.Copyright), [selection policy](rime-data/DATA-SOURCES.md) |
| SQLite | vendored amalgamation; version in `third_party/sqlite/sqlite3.h` | [Public domain](https://www.sqlite.org/copyright.html); original dedication preserved in the source |
| GCC libstdc++ / libgcc | WinLibs GCC 16.2.0 | [GPL-3.0](licenses/gcc-gpl-3.0.txt) with [GCC Runtime Library Exception 3.1](licenses/gcc-runtime-exception.txt) |
| MinGW-w64 / winpthreads | WinLibs MinGW-w64 14.0.0 | [MinGW notices](licenses/mingw-w64.txt), [winpthreads notices](licenses/winpthreads.txt) |

Corresponding toolchain source is available from [WinLibs](https://github.com/brechtsanders/winlibs_mingw/releases),
[GCC](https://gcc.gnu.org/git/?p=gcc.git) and [MinGW-w64](https://github.com/mingw-w64/mingw-w64).
The runtime library code is unmodified. A build-local copy of the static
winpthreads archive omits its DLL version resource. Build-only changes to yaml-cpp, OpenCC and
marisa are recorded in `cmake/librime.cmake`: missing standard headers, static
library configuration and unused OpenCC data targets. A build-local copy of the
librime archive omits its version resource so the application can use its own.

## Downloaded directly from upstream during setup

The Windows ZIP does **not** redistribute sherpa-onnx SDK archives or model
weights. `tools/dependencies.json` pins the official download URLs, archive
SHA-256 and required-file SHA-256. `tools/download_models.ps1` verifies them
before installation. Downloaded upstream READMEs are preserved.

| Asset | Source and licensing information |
| --- | --- |
| sherpa-onnx Windows x64 runtime | [v2.0.1](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v2.0.1), [Apache-2.0](licenses/sherpa-onnx.txt); includes [ONNX Runtime](https://github.com/microsoft/onnxruntime), [MIT](licenses/onnxruntime.txt) and its upstream dependencies |
| Bilingual streaming Zipformer, 2023-02-20 INT8 | [Original publisher's pinned model card](https://huggingface.co/pfluo/k2fsa-zipformer-chinese-english-mixed/blob/6eb615ae77ecac05c5628d5c8ed7037c14a338d5/README.md), Apache-2.0; converted package from sherpa-onnx |
| Qwen3-ASR 0.6B INT8, 2026-03-25 | [Qwen official model](https://huggingface.co/Qwen/Qwen3-ASR-0.6B), Apache-2.0; [ONNX conversion](https://github.com/Wasser1462/Qwen3-ASR-onnx), package from sherpa-onnx, default final recognizer |
| Final Zipformer CTC, 2025-07-03 INT8 | [Publisher's model card](https://huggingface.co/csukuangfj/sherpa-onnx-zipformer-ctc-zh-int8-2025-07-03), Apache-2.0; [maintainer clarification](https://github.com/k2-fsa/sherpa-onnx/issues/4065) |
| CT-Transformer punctuation, 2024-04-12 INT8 | [Original ModelScope model](https://modelscope.cn/models/iic/punc_ct-transformer_zh-cn-common-vocab272727-pytorch), Apache-2.0; converted package from sherpa-onnx |

Microsoft Visual C++ Redistributable is a separate prerequisite downloaded from
[Microsoft](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
It is not redistributed in this repository or ZIP.

## Visual references

The three native skins are original implementations. Design research referred to
[Rime Skin Studio](https://github.com/JGCindy/rime-weasel-skin-studio),
[Catppuccin / Fcitx5](https://github.com/catppuccin/fcitx5) and
[Fluent / Fcitx5](https://github.com/Reverier-Xu/Fluent-fcitx5).
Their theme code, assets and fonts are not bundled. No WeChat Input Method code,
models, dictionaries or assets are included.
