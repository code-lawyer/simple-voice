#include "harness.hpp"

void Utf8CountsCharactersNotBytes();
void FirstSentenceAndSingleCountDoNotReplace();
void PreferredHomophoneReplacesOnlySamePinyin();
void ExpiredEntriesDropOut();
void CutsANameIntoTokensWithPinyin();

int main(int argc, char** argv) {
  const Test tests[] = {
      {"Utf8CountsCharactersNotBytes", Utf8CountsCharactersNotBytes},
      {"lexicon.FirstSentenceAndSingleCountDoNotReplace", FirstSentenceAndSingleCountDoNotReplace},
      {"lexicon.PreferredHomophoneReplacesOnlySamePinyin", PreferredHomophoneReplacesOnlySamePinyin},
      {"lexicon.ExpiredEntriesDropOut", ExpiredEntriesDropOut},
      {"segmenter.CutsANameIntoTokensWithPinyin", CutsANameIntoTokensWithPinyin},
  };
  return RunTests(argc, argv, tests, sizeof(tests) / sizeof(tests[0]));
}
