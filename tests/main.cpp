#include "harness.hpp"

void Utf8CountsCharactersNotBytes();
void FirstSentenceAndSingleCountDoNotReplace();
void PreferredHomophoneReplacesOnlySamePinyin();
void ExpiredEntriesDropOut();
void CutsANameIntoTokensWithPinyin();
void SilenceAfterSpeechEndpointsAt500ms();
void SixtySecondsForcesEndpoint();
void Resample48kTo16kKeepsDuration();

int main(int argc, char** argv) {
  const Test tests[] = {
      {"Utf8CountsCharactersNotBytes", Utf8CountsCharactersNotBytes},
      {"lexicon.FirstSentenceAndSingleCountDoNotReplace", FirstSentenceAndSingleCountDoNotReplace},
      {"lexicon.PreferredHomophoneReplacesOnlySamePinyin", PreferredHomophoneReplacesOnlySamePinyin},
      {"lexicon.ExpiredEntriesDropOut", ExpiredEntriesDropOut},
      {"segmenter.CutsANameIntoTokensWithPinyin", CutsANameIntoTokensWithPinyin},
      {"vad.SilenceAfterSpeechEndpointsAt500ms", SilenceAfterSpeechEndpointsAt500ms},
      {"vad.SixtySecondsForcesEndpoint", SixtySecondsForcesEndpoint},
      {"vad.Resample48kTo16kKeepsDuration", Resample48kTo16kKeepsDuration},
  };
  return RunTests(argc, argv, tests, sizeof(tests) / sizeof(tests[0]));
}
