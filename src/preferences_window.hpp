#pragma once
#include "settings.hpp"
#include "lexicon.hpp"
#include "ui_theme.hpp"
#include "model_settings.hpp"
#include <functional>
#include <vector>

struct AudioDevice { std::wstring id, name; };
std::vector<AudioDevice> InputDevices();
bool CopyUserText(HWND owner,const std::wstring& text);

enum class ModelTaskState { Idle, Running, Queued };

struct PreferencesActions {
  std::function<bool(const InputPreferences&)> save;
  std::function<bool(UiSkin)> skin;
  std::function<void()> hotkey;
  std::function<void()> retry_models;
  std::function<std::wstring()> diagnostics;
  std::function<std::string(const std::string&)> input_code;
  std::function<bool()> terms_changed;
  std::function<ModelSettings()> models;
  std::function<std::wstring(SpeechModel)> model_status;
  std::function<std::wstring(const ModelSettings&)> use_model;
  std::function<ModelTaskState()> model_task;
  std::function<void()> cancel_model_operation;
  std::function<std::wstring(const std::wstring&)> backup;
  std::function<std::wstring(const std::wstring&)> restore;
  std::function<std::wstring(int)> practice;
  std::function<std::wstring()> practice_status;
};

class PreferencesWindow {
 public:
  ~PreferencesWindow();
  void Open(Lexicon* lexicon,InputPreferences preferences,UiSkin skin,
            PreferencesActions actions,int page=0);
  bool Translate(MSG* message);
  void Close();
  void ReloadInputs(InputPreferences preferences,UiSkin skin);
  HWND hwnd() const { return hwnd_; }
 private:
  struct Control { HWND hwnd; int page,x,y,w,h; };
  HWND hwnd_=nullptr;
  HFONT font_=nullptr,heading_=nullptr;
  HBRUSH brush_=nullptr;
  UINT dpi_=96;
  int page_=0,offset_=0;
  int scroll_=0;
  UiSkin skin_=UiSkin::Titanium;
  InputPreferences preferences_;
  PreferencesActions actions_;
  Lexicon* lexicon_=nullptr;
  std::vector<Control> controls_;
  std::vector<Entry> entries_;
  std::vector<AudioDevice> devices_;
  std::string editing_;
  Entry deleted_{};
  bool have_undo_=false,changing_=false;
  HWND Add(const wchar_t* type,const wchar_t* text,int id,int page,int x,int y,int w,int h,DWORD style=0);
  void Layout();
  void Scroll(int position);
  void Page(int page);
  void Search();
  void Devices();
  void Models();
  ModelSettings SelectedModel() const;
  void Command(int id,int notification);
  void Status(const std::wstring& text);
  std::wstring Text(int id) const;
  void Set(int id,const std::wstring& text);
  static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
};

class PreviewEditor {
 public:
  ~PreviewEditor();
  void Open(const std::string& current,const std::string& original,std::function<void(std::string)> save,bool insert_uncertain=false);
  void Close();
  bool Translate(MSG* message);
  HWND hwnd() const { return hwnd_; }
 private:
  HWND hwnd_=nullptr,edit_=nullptr,status_=nullptr,restore_=nullptr,copy_=nullptr,save_=nullptr;
  HFONT font_=nullptr;
  UINT dpi_=96;
  std::string original_;
  std::function<void(std::string)> on_save_;
  void Layout();
  static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
};
