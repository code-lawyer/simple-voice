#include "tsf_service.hpp"

#include <msctf.h>

namespace {

HINSTANCE g_module = nullptr;

}  // namespace

HINSTANCE VoiceImeModule() { return g_module; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
  if (object == nullptr) return E_POINTER;
  *object = nullptr;
  if (!IsEqualGUID(clsid, CLSID_VoiceIme)) return CLASS_E_CLASSNOTAVAILABLE;
  return CreateVoiceImeFactory(iid, object);
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }
