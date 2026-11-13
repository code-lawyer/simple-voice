#include "utf8.hpp"

int CodePointCount(std::string_view utf8) {
  int count = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    std::size_t need = 1;
    if ((lead & 0x80) == 0) {
      need = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      need = 4;
    } else {
      ++count;
      ++i;
      continue;
    }
    if (i + need > utf8.size()) {
      count += static_cast<int>(utf8.size() - i);
      break;
    }
    bool ok = true;
    for (std::size_t j = 1; j < need; ++j) {
      const auto cont = static_cast<unsigned char>(utf8[i + j]);
      if ((cont & 0xC0) != 0x80) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      ++count;
      ++i;
      continue;
    }
    ++count;
    i += need;
  }
  return count;
}
