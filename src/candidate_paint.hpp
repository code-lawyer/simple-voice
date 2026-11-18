#pragma once
#include "ui_theme.hpp"
#include "utf8.hpp"
#include <string>
#include <vector>

// Both the live popup and the settings sample draw in local strip coordinates.
inline void PaintCandidateStrip(HDC dc,const RECT& client,UiSkin skin,UINT dpi,
                                const std::vector<RECT>& items,const std::vector<std::string>& words,
                                int highlight,int hover) {
  const auto colors=UiPalette(skin);
  UiSurface(dc,client,colors,dpi);
  SetBkMode(dc,TRANSPARENT);
  HFONT font=UiFont(16,dpi);
  HFONT number=UiFont(12,dpi);
  auto old=SelectObject(dc,font);
  for (std::size_t i=0;i<items.size();++i) {
    RECT item=items[i];
    const bool selected=static_cast<int>(i)==highlight;
    const bool highlight_ink=selected || (colors.high_contrast && hover==static_cast<int>(i));
    if (selected || hover==static_cast<int>(i)) {
      HBRUSH brush=CreateSolidBrush(selected ? colors.selected : colors.hover);
      auto old_brush=SelectObject(dc,brush); auto old_pen=SelectObject(dc,GetStockObject(NULL_PEN));
      const int round=UiPx(skin==UiSkin::Daylight ? 18 : 12,dpi);
      RoundRect(dc,item.left,item.top,item.right,item.bottom,round,round);
      SelectObject(dc,old_pen); SelectObject(dc,old_brush); DeleteObject(brush);
      if (selected && skin==UiSkin::Titanium && !colors.high_contrast)
        UiFill(dc,{item.left+UiPx(5,dpi),item.bottom-UiPx(2,dpi),item.right-UiPx(5,dpi),item.bottom-UiPx(1,dpi)},colors.accent);
    }
    item.left+=UiPx(8,dpi); item.right-=UiPx(5,dpi);
    SelectObject(dc,number);
    SetTextColor(dc,highlight_ink ? colors.selected_text : colors.secondary);
    RECT index_rect=item; index_rect.right=std::min(item.right,item.left+UiPx(15,dpi));
    DrawTextW(dc,std::to_wstring(i+1).c_str(),-1,&index_rect,DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc,font);
    item.left=index_rect.right;
    SetTextColor(dc,highlight_ink ? colors.selected_text : colors.text);
    const auto label=Utf8ToWide(words[i]);
    DrawTextW(dc,label.c_str(),-1,&item,DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
  RECT settings{client.right-UiPx(52,dpi),UiPx(5,dpi),client.right-UiPx(3,dpi),client.bottom-UiPx(5,dpi)};
  if (hover==5) UiFill(dc,settings,colors.hover);
  UiFill(dc,{settings.left,UiPx(13,dpi),settings.left+std::max(1,UiPx(1,dpi)),client.bottom-UiPx(13,dpi)},colors.rule);
  SelectObject(dc,number);
  SetTextColor(dc,colors.high_contrast && hover==5 ? colors.selected_text : colors.secondary);
  DrawTextW(dc,L"设置",-1,&settings,DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
  SelectObject(dc,old); DeleteObject(number); DeleteObject(font);
}
