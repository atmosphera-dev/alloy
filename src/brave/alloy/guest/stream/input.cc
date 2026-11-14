// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/input.h"
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include "cef/include/cef_parser.h"

namespace alloy::guest::stream {
CefRefPtr<CefDictionaryValue> ParseDetails(const std::string& json) {
  auto value = CefParseJSON(json, JSON_PARSER_RFC);
  return value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary()->Copy(false) : nullptr;
}
std::string WriteDetails(CefRefPtr<CefDictionaryValue> dictionary) {
  auto value = CefValue::Create(); value->SetDictionary(dictionary);
  return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}
bool ValidUtf16(const CefString& text) {
  const auto* value = text.GetStruct();
  if (!value) return true;
  for (size_t i = 0; i < value->length; ++i) {
    uint16_t unit = value->str[i];
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++i == value->length || value->str[i] < 0xdc00 || value->str[i] > 0xdfff) return false;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) { return false; }
  }
  return true;
}
namespace {
class Interaction final : public AlloyGuestInteraction {
 public:
  Interaction(Owner owner, InteractionData data) : owner_(owner), data_(std::move(data)) {}
  uint64_t GetId() override { return data_.id; }
  CefString GetSource() override { return data_.source; }
  CefString GetKind() override { return data_.kind; }
  uint64_t GetViewId() override { return owner_.view; }
  uint64_t GetDocumentId() override { return data_.document; }
  uint64_t GetGeneration() override { return data_.generation; }
  uint64_t GetSurface() override { return data_.surface; }
  uint64_t GetAppCaptureTimeUs() override { return data_.capture_us; }
  uint64_t GetDispatchTimeUs() override { return data_.dispatch_us; }
  CefString GetOutcome() override { return data_.outcome; }
  CefRefPtr<CefDictionaryValue> GetDetails() override { return ParseDetails(data_.details); }
 private:
  const Owner owner_;
  const InteractionData data_;
  IMPLEMENT_REFCOUNTING(Interaction);
};
bool Integer(CefRefPtr<CefDictionaryValue> data, const char* name, int* result) {
  if (data->GetType(name) != VTYPE_INT) return false;
  *result = data->GetInt(name); return true;
}
bool Number(CefRefPtr<CefDictionaryValue> data, const char* name, double* result) {
  if (data->GetType(name) == VTYPE_INT) *result = data->GetInt(name);
  else if (data->GetType(name) == VTYPE_DOUBLE) *result = data->GetDouble(name);
  else return false;
  return std::isfinite(*result) && std::isfinite(static_cast<float>(*result));
}
bool Boolean(CefRefPtr<CefDictionaryValue> data, const char* name, bool* result) {
  if (data->GetType(name) != VTYPE_BOOL) return false;
  *result = data->GetBool(name); return true;
}
bool Unsigned(CefRefPtr<CefDictionaryValue> data, const char* name, uint32_t* result) {
  double number = 0;
  if (!Number(data, name, &number) || number < 0 || number > std::numeric_limits<uint32_t>::max() ||
      std::floor(number) != number) return false;
  *result = static_cast<uint32_t>(number); return true;
}
bool Range(CefRefPtr<CefDictionaryValue> data, const char* name, cef_range_t* result) {
  auto value = data->GetList(name);
  if (!value || value->GetSize() != 2) return false;
  double range[2] = {};
  for (size_t i = 0; i < 2; ++i) {
    if (value->GetType(i) == VTYPE_INT) range[i] = value->GetInt(i);
    else if (value->GetType(i) == VTYPE_DOUBLE) range[i] = value->GetDouble(i);
    else return false;
    if (!std::isfinite(range[i]) || std::floor(range[i]) != range[i]) return false;
  }
  if (range[0] == -1 && range[1] == -1) { *result = CefRange(-1, -1); return true; }
  if (range[0] < 0 || range[1] < 0 || range[0] >= std::numeric_limits<uint32_t>::max() ||
      range[1] >= std::numeric_limits<uint32_t>::max()) return false;
  *result = CefRange(static_cast<uint32_t>(range[0]), static_cast<uint32_t>(range[1]));
  return true;
}
bool Contains(const CefRect& rectangle, double x, double y) {
  return x >= rectangle.x && y >= rectangle.y &&
      x < static_cast<double>(rectangle.x) + rectangle.width &&
      y < static_cast<double>(rectangle.y) + rectangle.height;
}
}
CefRefPtr<AlloyGuestInteraction> MakeInteraction(Owner owner, InteractionData data) {
  return new Interaction(owner, std::move(data));
}
void Input::Dispatch(Owner owner, uint64_t generation, const CefRect& view,
                      bool visible, const CefRect& popup, CefRefPtr<CefBrowser> browser,
                      InteractionData input, std::function<uint64_t()> clock, Receipt receipt) {
  auto reject = [&](const char* reason) {
    input.dispatch_us = clock(); input.outcome = std::string("rejected:") + reason;
    receipt(owner, std::move(input));
  };
  if (input.id <= last_id_) { reject("event-order"); return; }
  last_id_ = input.id;
  if (input.document != owner.document || input.generation != generation) {
    reject("stale-generation"); return;
  }
  if (!input.outcome.empty() || input.dispatch_us) { reject("invalid"); return; }
  auto data = ParseDetails(input.details);
  if (!data || !browser) { reject("invalid"); return; }
  auto host = browser->GetHost();
  std::function<void()> dispatch;
  int x = 0, y = 0;
  uint32_t modifiers = 0;
  double root_x = 0, root_y = 0;
  auto point = [&](double px, double py, bool leave) {
    if (!std::isfinite(px) || !std::isfinite(py) || input.surface > 1) return false;
    if (input.surface) {
      if (!visible || (!leave && (px < 0 || py < 0 || px >= popup.width || py >= popup.height))) return false;
      px += popup.x; py += popup.y;
    } else if (!leave && (!Contains(view, px, py) || (visible && Contains(popup, px, py)))) {
      return false;
    }
    if (px < std::numeric_limits<int>::min() || px > std::numeric_limits<int>::max() ||
        py < std::numeric_limits<int>::min() || py > std::numeric_limits<int>::max()) return false;
    root_x = px; root_y = py; x = px; y = py; return true;
  };
  if (input.kind == "pointer" || input.kind == "wheel") {
    int px = 0, py = 0;
    auto action = data->GetString("action").ToString();
    if (!Integer(data, "x", &px) || !Integer(data, "y", &py) ||
        !Unsigned(data, "modifiers", &modifiers) ||
        !point(px, py, action == "leave" || (input.kind == "pointer" &&
            (action == "move" || action == "up") && !buttons_.empty()))) {
      reject("coordinates"); return;
    }
    CefMouseEvent event; event.x = x; event.y = y; event.modifiers = static_cast<uint32_t>(modifiers);
    if (input.kind == "wheel") {
      int dx = 0, dy = 0;
      if (!Integer(data, "delta_x", &dx) || !Integer(data, "delta_y", &dy)) { reject("invalid"); return; }
      dispatch = [host, event, dx, dy] { host->SendMouseWheelEvent(event, dx, dy); };
    } else if (action == "move" || action == "leave") {
      dispatch = [host, event, leave = action == "leave"] { host->SendMouseMoveEvent(event, leave); };
    } else if (action == "down" || action == "up") {
      int button = 0, clicks = 0;
      if (!Integer(data, "button", &button) || button < MBT_LEFT || button > MBT_RIGHT ||
          !Integer(data, "clicks", &clicks) || clicks < 1) { reject("invalid"); return; }
      if (action == "down") buttons_.insert(button);
      else buttons_.erase(button);
      dispatch = [host, event, button, clicks, up = action == "up"] {
        host->SendMouseClickEvent(event, static_cast<cef_mouse_button_type_t>(button), up, clicks);
      };
    }
  } else if (input.kind == "key") {
    CefKeyEvent event;
    int type = 0, character = 0, unmodified = 0;
    bool system = false, editable = false;
    if (!Integer(data, "type", &type) || type < KEYEVENT_RAWKEYDOWN || type > KEYEVENT_CHAR ||
        !Unsigned(data, "modifiers", &modifiers) ||
        !Integer(data, "windows_key_code", &event.windows_key_code) ||
        !Integer(data, "native_key_code", &event.native_key_code) ||
        !Integer(data, "character", &character) || character < 0 || character > 65535 ||
        !Integer(data, "unmodified_character", &unmodified) || unmodified < 0 || unmodified > 65535 ||
        !Boolean(data, "system", &system) || !Boolean(data, "editable", &editable)) {
      reject("invalid"); return;
    }
    event.type = static_cast<cef_key_event_type_t>(type); event.modifiers = static_cast<uint32_t>(modifiers);
    event.character = character; event.unmodified_character = unmodified;
    event.is_system_key = system; event.focus_on_editable_field = editable;
    dispatch = [host, event] { host->SendKeyEvent(event); };
  } else if (input.kind == "touch") {
    CefTouchEvent event;
    int type = 0, pointer = 0;
    double px = 0, py = 0, rx = 0, ry = 0, rotation = 0, pressure = 0;
    if (!Integer(data, "id", &event.id) || event.id == -1 ||
        !Integer(data, "type", &type) || type < CEF_TET_RELEASED || type > CEF_TET_CANCELLED ||
        !Integer(data, "pointer_type", &pointer) || pointer < CEF_POINTER_TYPE_TOUCH || pointer > CEF_POINTER_TYPE_UNKNOWN ||
        !Unsigned(data, "modifiers", &modifiers) || !Number(data, "x", &px) || !Number(data, "y", &py) ||
        !Number(data, "radius_x", &rx) || rx < 0 || !Number(data, "radius_y", &ry) || ry < 0 ||
        !Number(data, "rotation", &rotation) || !Number(data, "pressure", &pressure) || pressure < 0 || pressure > 1 ||
        !point(px, py, type != CEF_TET_PRESSED && touches_.contains(event.id))) { reject("invalid"); return; }
    if ((type == CEF_TET_PRESSED && (touches_.contains(event.id) || touches_.size() >= 16)) ||
        (type != CEF_TET_PRESSED && !touches_.contains(event.id))) { reject("touch-order"); return; }
    if (type == CEF_TET_PRESSED) touches_.insert(event.id);
    else if (type != CEF_TET_MOVED) touches_.erase(event.id);
    event.type = static_cast<cef_touch_event_type_t>(type); event.pointer_type = static_cast<cef_pointer_type_t>(pointer);
    event.x = root_x; event.y = root_y;
    event.radius_x = rx; event.radius_y = ry; event.rotation_angle = rotation; event.pressure = pressure;
    event.modifiers = static_cast<uint32_t>(modifiers);
    dispatch = [host, event] { host->SendTouchEvent(event); };
  } else if (input.kind == "focus") {
    bool focused = false;
    if (!Boolean(data, "focused", &focused)) { reject("invalid"); return; }
    dispatch = [host, focused] { host->SetFocus(focused); };
  } else if (input.kind == "capture-lost") {
    dispatch = [host] { host->SendCaptureLostEvent(); };
  } else if (input.kind == "ime-set" || input.kind == "ime-commit") {
    if (data->GetType("text") != VTYPE_STRING) { reject("invalid"); return; }
    auto text = data->GetString("text");
    CefRange replacement;
    if (!ValidUtf16(text) || !Range(data, "replacement", &replacement)) { reject("invalid"); return; }
    if (input.kind == "ime-commit") {
      int relative = 0;
      if (!Integer(data, "relative_cursor", &relative)) { reject("invalid"); return; }
      dispatch = [host, text, replacement, relative] { host->ImeCommitText(text, replacement, relative); };
    } else {
      CefRange selection;
      auto lines = data->GetList("underlines");
      if (!Range(data, "selection", &selection) || selection.from > text.length() ||
          selection.to > text.length() || !lines) { reject("invalid"); return; }
      std::vector<CefCompositionUnderline> underlines;
      for (size_t i = 0; i < lines->GetSize(); ++i) {
        auto line = lines->GetDictionary(i);
        CefCompositionUnderline underline;
        uint32_t color = 0, background = 0; int style = 0; bool thick = false;
        if (!line || !Range(line, "range", &underline.range) || underline.range.from >= underline.range.to ||
            underline.range.to > text.length() || !Unsigned(line, "color", &color) ||
            !Unsigned(line, "background", &background) || !Integer(line, "style", &style) ||
            style < CEF_CUS_SOLID || style >= CEF_CUS_NUM_VALUES || !Boolean(line, "thick", &thick)) {
          reject("invalid"); return;
        }
        underline.color = static_cast<uint32_t>(color); underline.background_color = static_cast<uint32_t>(background);
        underline.style = static_cast<cef_composition_underline_style_t>(style); underline.thick = thick;
        underlines.push_back(underline);
      }
      dispatch = [host, text, underlines, replacement, selection] {
        host->ImeSetComposition(text, underlines, replacement, selection);
      };
    }
  } else if (input.kind == "ime-finish") {
    bool keep = false;
    if (!Boolean(data, "keep_selection", &keep)) { reject("invalid"); return; }
    dispatch = [host, keep] { host->ImeFinishComposingText(keep); };
  } else if (input.kind == "ime-cancel") {
    dispatch = [host] { host->ImeCancelComposition(); };
  }
  if (!dispatch) { reject("invalid"); return; }
  // Queue the receipt before the native call can synchronously change owners.
  // The host cannot transmit the queue until this native dispatch returns.
  input.dispatch_us = clock(); input.outcome = "accepted";
  receipt(owner, std::move(input));
  dispatch();
}
}  // namespace alloy::guest::stream
