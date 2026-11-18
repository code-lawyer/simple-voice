#pragma once
#include "tsf_service.hpp"
#include "tsf_indicator_compat.hpp"
#include "key_route.hpp"

// A separately owned COM object: the shell may retain it after TIP deactivation.
// Detach severs all callbacks before removing it from the language bar.
class InputIndicator final : public ITfLangBarItemButton, public ITfSource {
 public:
  using Toggle=bool(*)(void*);
  using Enabled=bool(*)(void*);
  InputIndicator(HINSTANCE module,Toggle toggle,void* owner,InputMode mode,Enabled enabled=nullptr);
  HRESULT Attach(ITfThreadMgr* manager);
  void Detach();
  void SetMode(InputMode mode, bool ascii_punctuation = false);
  void SetEnabled(bool enabled);
  void RefreshIcon();
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO*) override;
  HRESULT STDMETHODCALLTYPE GetStatus(DWORD*) override;
  HRESULT STDMETHODCALLTYPE Show(BOOL) override;
  HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR*) override;
  HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick,POINT,const RECT*) override;
  HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu*) override;
  HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT) override;
  HRESULT STDMETHODCALLTYPE GetIcon(HICON*) override;
  HRESULT STDMETHODCALLTYPE GetText(BSTR*) override;
  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID,IUnknown*,DWORD*) override;
  HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD) override;
 private:
  ~InputIndicator();
  void Notify(DWORD flags);
  LONG refs_=1;
  HINSTANCE module_;
  Toggle toggle_;
  Enabled query_enabled_;
  void* owner_;
  InputMode mode_;
  bool ascii_punctuation_ = false;
  bool enabled_=true;
  ITfLangBarItemMgr* manager_=nullptr;
  ITfLangBarItemSink* sink_=nullptr;
  DWORD cookie_=0;
};
