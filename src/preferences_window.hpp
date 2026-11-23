#pragma once
#include "settings.hpp"
#include "lexicon.hpp"
#include "ui_theme.hpp"
#include "model_settings.hpp"
#include "review_window.hpp"
#include <commctrl.h>
#include <functional>
#include <optional>
#include <vector>

struct AudioDevice { std::wstring id, name; bool available=true; };
std::vector<AudioDevice> InputDevices();
bool CopyUserText(HWND owner,const std::wstring& text);

enum class ModelTaskState { Idle, Running, Queued };
struct ModelActionResult { std::wstring message; bool pending=false; };
struct PracticeView {
  std::wstring text;
  bool can_start=false,can_finalize=false,can_confirm=false,can_cancel=false;
};

struct PreferencesActions {
  std::function<bool(const InputPreferences&)> save;
  std::function<bool(UiSkin)> skin;
  std::function<void()> hotkey;
  std::function<std::wstring()> hotkey_label;
  std::function<void()> retry_models;
  std::function<std::wstring()> diagnostics;
  std::function<std::string(const std::string&)> input_code;
  std::function<bool()> terms_changed;
  std::function<std::wstring(const std::vector<ReviewChoice>&)> accept_review;
  std::function<bool()> review_failed;
  std::function<ModelSettings()> models;
  std::function<bool(bool)> save_model_idle_release;
  bool preload_on_activate = true;
  std::function<bool(bool)> save_preload_on_activate;
  std::function<std::wstring(SpeechModel)> model_status;
  std::function<ModelActionResult(const ModelSettings&)> use_model;
  std::function<ModelTaskState()> model_task;
  std::function<void()> cancel_model_operation;
  std::function<std::wstring(const std::wstring&)> backup;
  std::function<std::wstring(const std::wstring&)> restore;
  std::function<std::wstring(int)> practice;
  std::function<PracticeView()> practice_status;
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
  enum class Face { Body, Detail, Meta, Section, Title };
  struct FormRow { int page,gap; };
  struct Control { HWND hwnd; int page,x,row,w,h; Face face; bool combo; };
  struct ModelFeedback { SpeechModel model; std::wstring completion; };
  HWND hwnd_=nullptr,pane_=nullptr;
  HFONT font_=nullptr,heading_=nullptr,section_=nullptr,meta_=nullptr,icons_=nullptr;
  HICON mark_=nullptr;
  UiColors colors_{};
  UINT dpi_=96;
  int page_=0,offset_=0;
  int scroll_=0,rail_=0,footer_=0,hover_=-1;
  bool compact_=false,always_cues_=false;
  UiSkin skin_=UiSkin::Titanium;
  InputPreferences preferences_;
  PreferencesActions actions_;
  Lexicon* lexicon_=nullptr;
  WordReviewWindow review_window_;
  std::vector<Control> controls_;
  std::vector<FormRow> rows_;
  std::vector<Entry> entries_;
  std::vector<AudioDevice> devices_;
  std::wstring terms_error_;
  std::string editing_;
  std::string displayed_model_hints_;
  Entry deleted_{};
  std::optional<ModelFeedback> pending_model_feedback_;
  bool have_undo_=false,changing_=false;
  HWND Add(const wchar_t* type,const wchar_t* text,int id,int page,int x,int row,int w,int h,
           DWORD style=0,Face face=Face::Body);
  void BuildForm(const ModelSettings& models);
  void UpdateAppearance();
  void FitWindow(const RECT* suggested);
  void Layout();
  void LayoutForm();
  void FitOutput(int id);
  bool ShowAccessKeys() const;
  void RevealFocus();
  void PaintFrame(HDC dc);
  LRESULT DrawItem(const DRAWITEMSTRUCT& item);
  LRESULT DrawButton(const NMCUSTOMDRAW& item);
  void Scroll(int position);
  void Page(int page);
  void RefreshPage();
  const Entry* SelectedTerm() const;
  void Search();
  void RefreshTerms(const wchar_t* success,const wchar_t* export_failure);
  const AudioDevice* SelectedDevice() const;
  void Devices(bool preserve_selection=false);
  void Models();
  void UpdatePractice();
  void SyncModelHints();
  void PollModelFeedback();
  ModelSettings SelectedModel() const;
  void Command(int id,int notification);
  void Status(const std::wstring& text);
  std::wstring Text(int id) const;
  void Set(int id,const std::wstring& text);
  static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
  static LRESULT CALLBACK PaneProc(HWND,UINT,WPARAM,LPARAM);
  static LRESULT CALLBACK NavigationProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
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
