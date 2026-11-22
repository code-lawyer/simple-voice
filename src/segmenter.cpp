#include "segmenter.hpp"

#include "cppjieba/MixSegment.hpp"
#include "utf8.hpp"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace {

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  if (last == '/' || last == '\\') return dir + name;
  return dir + "/" + name;
}

bool Decode(const std::string& text, std::size_t& index, char32_t& cp) {
  if (index >= text.size()) return false;
  const auto lead = static_cast<unsigned char>(text[index]);
  if (lead < 0x80) {
    cp = lead;
    ++index;
    return true;
  }
  int need = 0;
  char32_t value = 0;
  if ((lead & 0xE0) == 0xC0) {
    need = 1;
    value = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    need = 2;
    value = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    need = 3;
    value = lead & 0x07;
  } else {
    cp = lead;
    ++index;
    return true;
  }
  if (index + static_cast<std::size_t>(need) >= text.size()) {
    cp = lead;
    ++index;
    return true;
  }
  for (int i = 1; i <= need; ++i) {
    const auto cont = static_cast<unsigned char>(text[index + static_cast<std::size_t>(i)]);
    if ((cont & 0xC0) != 0x80) {
      cp = lead;
      ++index;
      return true;
    }
    value = (value << 6) | (cont & 0x3F);
  }
  cp = value;
  index += static_cast<std::size_t>(need) + 1;
  return true;
}

char Untone(char32_t cp) {
  switch (cp) {
    case 'a':
    case 'A':
    case 0x0101:
    case 0x00E1:
    case 0x01CE:
    case 0x00E0:
      return 'a';
    case 'e':
    case 'E':
    case 0x0113:
    case 0x00E9:
    case 0x011B:
    case 0x00E8:
      return 'e';
    case 'i':
    case 'I':
    case 0x012B:
    case 0x00ED:
    case 0x01D0:
    case 0x00EC:
      return 'i';
    case 'o':
    case 'O':
    case 0x014D:
    case 0x00F3:
    case 0x01D2:
    case 0x00F2:
      return 'o';
    case 'u':
    case 'U':
    case 0x016B:
    case 0x00FA:
    case 0x01D4:
    case 0x00F9:
      return 'u';
    case 0x00FC:
    case 0x01D6:
    case 0x01D8:
    case 0x01DA:
    case 0x01DC:
      return 'v';
    case 'n':
    case 'N':
    case 0x0144:
    case 0x0148:
    case 0x01F9:
      return 'n';
    case 'b':
    case 'B':
      return 'b';
    case 'c':
    case 'C':
      return 'c';
    case 'd':
    case 'D':
      return 'd';
    case 'f':
    case 'F':
      return 'f';
    case 'g':
    case 'G':
      return 'g';
    case 'h':
    case 'H':
      return 'h';
    case 'j':
    case 'J':
      return 'j';
    case 'k':
    case 'K':
      return 'k';
    case 'l':
    case 'L':
      return 'l';
    case 'm':
    case 'M':
      return 'm';
    case 'p':
    case 'P':
      return 'p';
    case 'q':
    case 'Q':
      return 'q';
    case 'r':
    case 'R':
      return 'r';
    case 's':
    case 'S':
      return 's';
    case 't':
    case 'T':
      return 't';
    case 'w':
    case 'W':
      return 'w';
    case 'x':
    case 'X':
      return 'x';
    case 'y':
    case 'Y':
      return 'y';
    case 'z':
    case 'Z':
      return 'z';
    default:
      return 0;
  }
}

std::string UntoneSyllable(const std::string& syllable) {
  std::string plain;
  std::size_t index = 0;
  char32_t cp = 0;
  while (Decode(syllable, index, cp)) {
    const char ascii = Untone(cp);
    if (ascii != 0) plain.push_back(ascii);
  }
  return plain;
}

void LoadPinyin(const std::string& path, std::unordered_map<char32_t, std::string>& table) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 6 || line[0] != 'U' || line[1] != '+') continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const char32_t cp = static_cast<char32_t>(std::strtoul(line.c_str() + 2, nullptr, 16));
    std::size_t start = colon + 1;
    while (start < line.size() && line[start] == ' ') ++start;
    std::size_t stop = start;
    while (stop < line.size() && line[stop] != ',' && line[stop] != ' ' && line[stop] != '#') ++stop;
    if (stop == start) continue;
    const std::string plain = UntoneSyllable(line.substr(start, stop - start));
    if (!plain.empty()) table.emplace(cp, plain);
  }
}

std::string PinyinOf(const std::string& word, const std::unordered_map<char32_t, std::string>& table) {
  std::string pinyin;
  std::size_t index = 0;
  char32_t cp = 0;
  while (Decode(word, index, cp)) {
    const auto found = table.find(cp);
    if (found == table.end()) return "";
    if (!pinyin.empty()) pinyin.push_back(' ');
    pinyin += found->second;
  }
  return pinyin;
}

}  // namespace

struct Segmenter::Impl {
  std::mutex mutex;
  cppjieba::MixSegment jieba;
  std::unordered_map<char32_t, std::string> pinyin;

  explicit Impl(const std::string& dict_dir)
      : jieba(JoinPath(dict_dir, "jieba.dict.utf8"), JoinPath(dict_dir, "hmm_model.utf8"),
              JoinPath(dict_dir, "user.dict.utf8")) {
    LoadPinyin(JoinPath(dict_dir, "pinyin.txt"), pinyin);
  }
};

Segmenter::Segmenter(const std::string& dict_dir) : impl_(std::make_unique<Impl>(dict_dir)) {}

Segmenter::~Segmenter() = default;

std::vector<Token> Segmenter::Cut(const std::string& text) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::string> words;
  if (!text.empty()) impl_->jieba.Cut(text, words, true);
  if (words.empty()) {
    if (text.empty()) return {};
    return {Token{text, PinyinOf(text, impl_->pinyin)}};
  }
  std::vector<Token> tokens;
  tokens.reserve(words.size());
  for (const std::string& word : words) {
    tokens.push_back(Token{word, PinyinOf(word, impl_->pinyin)});
  }
  return tokens;
}
