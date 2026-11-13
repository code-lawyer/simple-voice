#include "harness.hpp"
#include "tsf_service.hpp"

#include <unknwn.h>

#include <string>

namespace {

std::wstring ExeDir() {
  wchar_t path[MAX_PATH];
  const DWORD count = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring text(path, count);
  const auto slash = text.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L"";
  text.resize(slash + 1);
  return text;
}

using FactoryFn = HRESULT(__stdcall*)(const GUID&, const GUID&, void**);

const GUID kFactoryId = {0x00000001, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

}  // namespace

void TsfFactoryExportsTheVoiceImeClass() {
  const HMODULE module = LoadLibraryW((ExeDir() + L"voiceime_tsf.dll").c_str());
  CHECK(module != nullptr);
  if (module == nullptr) return;
  auto factory = reinterpret_cast<FactoryFn>(GetProcAddress(module, "DllGetClassObject"));
  CHECK(factory != nullptr);
  if (factory == nullptr) {
    FreeLibrary(module);
    return;
  }
  const GUID wrong{};
  void* object = nullptr;
  CHECK(factory(wrong, kFactoryId, &object) == CLASS_E_CLASSNOTAVAILABLE);
  CHECK(object == nullptr);
  const HRESULT created = factory(CLSID_VoiceIme, kFactoryId, &object);
  CHECK(created == S_OK);
  CHECK(object != nullptr);
  if (object != nullptr) static_cast<IUnknown*>(object)->Release();
  FreeLibrary(module);
}
