#include <initguid.h>
#include "register_status.hpp"
#include "product.hpp"
#include "tsf_service.hpp"
#include "brand_resources.h"

#include <msctf.h>
#include <objbase.h>
#include <sddl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return "";
  const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(count > 0 ? count : 0, '\0');
  if (count > 0) {
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), count, nullptr, nullptr);
  }
  return utf8;
}

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return L"";
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(count > 0 ? count : 0, L'\0');
  if (count > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
  return wide;
}

std::wstring GuidText(const GUID& guid) {
  wchar_t text[64];
  const int count = StringFromGUID2(guid, text, 64);
  return count > 0 ? std::wstring(text) : L"";
}

std::wstring DllPath() {
  wchar_t exe[MAX_PATH];
  const DWORD count = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe, count);
  const auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L"voiceime_tsf.dll";
  path.resize(slash + 1);
  path += L"voiceime_tsf.dll";
  return path;
}

std::wstring CurrentUserSid() {
  HANDLE token=nullptr;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return L"";
  DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
  std::vector<BYTE> buffer(bytes);
  std::wstring result;
  if(bytes && GetTokenInformation(token,TokenUser,buffer.data(),bytes,&bytes)) {
    wchar_t* text=nullptr;
    if(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&text)) {
      result=text;LocalFree(text);
    }
  }
  CloseHandle(token);return result;
}

bool KeyAbsent(HKEY root,const std::wstring& path) {
  HKEY key=nullptr;const auto status=RegOpenKeyExW(root,path.c_str(),0,KEY_READ,&key);
  if(key)RegCloseKey(key);
  return status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND;
}

std::wstring ProfileKey() {
  return L"Software\\Microsoft\\CTF\\TIP\\"+GuidText(CLSID_VoiceIme)+
      L"\\LanguageProfile\\0x00000804\\"+GuidText(GUID_VoiceImeProfile);
}

bool RemoveCategory(ITfCategoryMgr* categories,const GUID& category) {
  const HRESULT result=categories->UnregisterCategory(CLSID_VoiceIme,category,CLSID_VoiceIme);
  const auto clsid=GuidText(CLSID_VoiceIme),cat=GuidText(category);
  const auto base=L"Software\\Microsoft\\CTF\\TIP\\"+clsid+L"\\Category\\";
  const bool absent=KeyAbsent(HKEY_LOCAL_MACHINE,base+L"Category\\"+cat+L"\\"+clsid) &&
      KeyAbsent(HKEY_LOCAL_MACHINE,base+L"Item\\"+clsid+L"\\"+cat);
  if(FAILED(result) && !absent)std::printf("category removal failed %08lx\n",static_cast<unsigned long>(result));
  return absent;
}

bool WriteString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const LONG result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool RegisterComServer(const std::wstring& dll) {
  const std::wstring clsid = GuidText(CLSID_VoiceIme);
  const std::wstring name = kProductDisplayName;
  const std::wstring classes = L"Software\\Classes\\CLSID\\" + clsid;
  bool ok = WriteString(HKEY_LOCAL_MACHINE, classes, nullptr, name);
  ok = WriteString(HKEY_LOCAL_MACHINE, classes + L"\\InProcServer32", nullptr, dll) && ok;
  ok = WriteString(HKEY_LOCAL_MACHINE, classes + L"\\InProcServer32", L"ThreadingModel", L"Apartment") && ok;
  return ok;
}

bool DeleteTree(HKEY root) {
  const std::wstring clsid = GuidText(CLSID_VoiceIme);
  const LSTATUS classes = RegDeleteTreeW(root, (L"Software\\Classes\\CLSID\\" + clsid).c_str());
  // Only this product's stable CLSID is removed. Personal data lives elsewhere.
  const LSTATUS tip = RegDeleteTreeW(root, (L"Software\\Microsoft\\CTF\\TIP\\" + clsid).c_str());
  auto gone = [](LSTATUS status) { return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND; };
  return gone(classes) && gone(tip);
}

bool InstallLayout(bool uninstall) {
  const std::wstring layout = L"0804:" + GuidText(CLSID_VoiceIme) + GuidText(GUID_VoiceImeProfile);
  const HMODULE input = LoadLibraryW(L"input.dll");
  if (input == nullptr) return false;
  using InstallFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
  auto install = reinterpret_cast<InstallFn>(GetProcAddress(input, "InstallLayoutOrTip"));
  const bool ok = install != nullptr && install(layout.c_str(), uninstall ? 1u : 0u);
  FreeLibrary(input);
  return ok;
}

bool RefreshProfile(const std::wstring& dll) {
  const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  if(FAILED(initialized))return false;
  ITfCategoryMgr* categories=nullptr;
  HRESULT result=CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfCategoryMgr,reinterpret_cast<void**>(&categories));
  if(SUCCEEDED(result)) {
    // Do not advertise AppContainer support: its data/broker paths have not
    // been validated. Remove the provisional capability from early rc.3 builds.
    if(!RemoveCategory(categories,kTipImmersiveCategory))result=E_FAIL;
    for(const GUID category : {kTipKeyboardCategory,GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
                              kTipSystemTrayCategory}) {
      if(FAILED(result))break;
      result=categories->RegisterCategory(CLSID_VoiceIme,category,CLSID_VoiceIme);
      if(FAILED(result))break;
    }
    categories->Release();
  }
  if(FAILED(result)) {
    CoUninitialize();
    std::printf("category refresh %08lx\n",static_cast<unsigned long>(result));
    return false;
  }
  ITfInputProcessorProfileMgr* profiles=nullptr;
  result=CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr,reinterpret_cast<void**>(&profiles));
  if(SUCCEEDED(result)) {
    const std::wstring name=kProductDisplayName;
    result=profiles->RegisterProfile(CLSID_VoiceIme,0x0804,GUID_VoiceImeProfile,name.c_str(),
        static_cast<ULONG>(name.size()),dll.c_str(),static_cast<ULONG>(dll.size()),
        static_cast<ULONG>(-IDI_SIMPLE_VOICE),nullptr,0,TRUE,0);
    profiles->Release();
  }
  CoUninitialize();
  std::printf("profile refresh %08lx\n",static_cast<unsigned long>(result));
  return SUCCEEDED(result);
}

bool EnableUserProfile() {
  if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return false;
  ITfInputProcessorProfiles* profiles=nullptr;
  HRESULT result=CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles,reinterpret_cast<void**>(&profiles));
  if(SUCCEEDED(result)) {
    result=profiles->EnableLanguageProfile(CLSID_VoiceIme,0x0804,GUID_VoiceImeProfile,TRUE);
    profiles->Release();
  }
  CoUninitialize();
  return SUCCEEDED(result);
}

bool UserProfileDisabled() {
  if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return false;
  ITfInputProcessorProfiles* profiles=nullptr;BOOL enabled=TRUE;
  HRESULT result=CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles,reinterpret_cast<void**>(&profiles));
  if(SUCCEEDED(result)) {
    result=profiles->IsEnabledLanguageProfile(CLSID_VoiceIme,0x0804,GUID_VoiceImeProfile,&enabled);
    profiles->Release();
  }
  CoUninitialize();
  return (SUCCEEDED(result) && !enabled) ||
      (KeyAbsent(HKEY_LOCAL_MACHINE,ProfileKey()) && KeyAbsent(HKEY_CURRENT_USER,ProfileKey()));
}

bool UnregisterProfile() {
  if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return false;
  bool ok=true;
  ITfInputProcessorProfileMgr* profiles=nullptr;
  if(SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr,reinterpret_cast<void**>(&profiles)))) {
    const auto result=profiles->UnregisterProfile(CLSID_VoiceIme,0x0804,GUID_VoiceImeProfile,0);
    ok=SUCCEEDED(result) || KeyAbsent(HKEY_LOCAL_MACHINE,ProfileKey());
    profiles->Release();
  } else ok=false;
  ITfCategoryMgr* categories=nullptr;
  if(SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfCategoryMgr,reinterpret_cast<void**>(&categories)))) {
    for(const GUID category : {kTipKeyboardCategory,GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
                              kTipSystemTrayCategory,kTipImmersiveCategory})
      ok=RemoveCategory(categories,category) && ok;
    categories->Release();
  } else ok=false;
  CoUninitialize();return ok;
}

}  // namespace

int main(int argc, char** argv) {
  bool uninstall=false;std::wstring expected_user;
  for(int i=1;i<argc;++i) {
    if(std::strcmp(argv[i],"--unregister")==0)uninstall=true;
    else if(std::strcmp(argv[i],"--expected-user-sid")==0 && i+1<argc)expected_user=Utf8ToWide(argv[++i]);
    else {std::fprintf(stderr,"Invalid registration argument\n");return 2;}
  }
  if(!expected_user.empty() && expected_user!=CurrentUserSid()) {
    std::fprintf(stderr,"Administrator elevation used a different Windows account; no registration was changed\n");
    return 2;
  }
  const std::wstring dll = DllPath();
  if (uninstall) {
    const bool layout=InstallLayout(true) || UserProfileDisabled();
    const bool profile=UnregisterProfile();
    if(!layout || !profile) {
      std::fprintf(stderr,"TSF cleanup failed; registration retained for retry\n");
      return UnregisterExitCode(false);
    }
    const bool hkcu = DeleteTree(HKEY_CURRENT_USER);
    const bool hklm = DeleteTree(HKEY_LOCAL_MACHINE);
    std::printf("unregistered\n");
    return UnregisterExitCode(hkcu && hklm);
  }
  const bool machine = RegisterComServer(dll) && RefreshProfile(dll);
  if(!machine) {
    std::printf("system registration failed\n");
    return RegisterExitCode(false,false);
  }
  // Earlier releases mirrored the full TIP tree under HKCU. Migrate only this
  // product's entries; TSF owns profile/category metadata and user enablement.
  // In particular, do not recreate the legacy tree after InstallLayoutOrTip.
  const bool user = DeleteTree(HKEY_CURRENT_USER) && InstallLayout(false) && EnableUserProfile();
  std::printf("system registration ok; current-user profile %s\n",user ? "enabled" : "failed");
  std::printf("dll %s\n", WideToUtf8(dll).c_str());
  return RegisterExitCode(user,machine);
}
