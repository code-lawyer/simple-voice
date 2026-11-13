#include "harness.hpp"

void Utf8CountsCharactersNotBytes();
void FirstSentenceAndSingleCountDoNotReplace();
void PreferredHomophoneReplacesOnlySamePinyin();
void ExpiredEntriesDropOut();
void CutsANameIntoTokensWithPinyin();
void RememberKeepsAUserWord();
void SilenceAfterSpeechEndpointsAt500ms();
void SixtySecondsForcesEndpoint();
void Resample48kTo16kKeepsDuration();
void SessionKeepsDraftUntilFinalThenEnterCommitsFinal();
void EscapeAndRestartDoNotCommit();
void MissingCaretGoesToBottomRight();
void DecodeWaitsForAFullFrame();
void EnterAndCaretRoundTrip();
void TsfFactoryExportsTheVoiceImeClass();

int main(int argc, char** argv) {
  const Test tests[] = {
      {"Utf8CountsCharactersNotBytes", Utf8CountsCharactersNotBytes},
      {"lexicon.FirstSentenceAndSingleCountDoNotReplace", FirstSentenceAndSingleCountDoNotReplace},
      {"lexicon.PreferredHomophoneReplacesOnlySamePinyin", PreferredHomophoneReplacesOnlySamePinyin},
      {"lexicon.ExpiredEntriesDropOut", ExpiredEntriesDropOut},
      {"segmenter.CutsANameIntoTokensWithPinyin", CutsANameIntoTokensWithPinyin},
      {"segmenter.RememberKeepsAUserWord", RememberKeepsAUserWord},
      {"vad.SilenceAfterSpeechEndpointsAt500ms", SilenceAfterSpeechEndpointsAt500ms},
      {"vad.SixtySecondsForcesEndpoint", SixtySecondsForcesEndpoint},
      {"vad.Resample48kTo16kKeepsDuration", Resample48kTo16kKeepsDuration},
      {"session.SessionKeepsDraftUntilFinalThenEnterCommitsFinal", SessionKeepsDraftUntilFinalThenEnterCommitsFinal},
      {"session.EscapeAndRestartDoNotCommit", EscapeAndRestartDoNotCommit},
      {"place.MissingCaretGoesToBottomRight", MissingCaretGoesToBottomRight},
      {"ipc.DecodeWaitsForAFullFrame", DecodeWaitsForAFullFrame},
      {"ipc.EnterAndCaretRoundTrip", EnterAndCaretRoundTrip},
      {"tsf.TsfFactoryExportsTheVoiceImeClass", TsfFactoryExportsTheVoiceImeClass},
  };
  return RunTests(argc, argv, tests, sizeof(tests) / sizeof(tests[0]));
}
