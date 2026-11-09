#include "portal-page.h"

#include <stdio.h>
#include <string.h>

#include "portal-strings.h"

namespace wattlink {

namespace {

// Appends text to a buffer and remembers when something did not fit. Once it fails, nothing more is written.
class Writer {
 public:
  Writer(char *out, size_t size) : out_(out), size_(size) {
    if (size_ > 0) {
      out_[0] = '\0';
    }
    ok_ = size_ > 0;
  }

  void add(const char *text) {
    if (!ok_) {
      return;
    }
    const size_t length = strlen(text);
    if (length_ + length + 1 > size_) {
      ok_ = false;
      return;
    }
    memcpy(out_ + length_, text, length + 1);
    length_ += length;
  }

  // Adds a number in decimal.
  void addNumber(size_t value) {
    char text[24];
    snprintf(text, sizeof(text), "%u", static_cast<unsigned>(value));
    add(text);
  }

  // The text is escaped before it is added.
  void addEscaped(const char *text) {
    if (!ok_) {
      return;
    }
    if (!escapeHtml(text, out_ + length_, size_ - length_)) {
      ok_ = false;
      return;
    }
    length_ += strlen(out_ + length_);
  }

  // Returns the length, or 0 with an empty text when anything did not fit.
  size_t finish() {
    if (!ok_) {
      if (size_ > 0) {
        out_[0] = '\0';
      }
      return 0;
    }
    return length_;
  }

 private:
  char *out_;
  size_t size_;
  size_t length_ = 0;
  bool ok_ = false;
};

const char *noticeText(Notice notice) {
  switch (notice) {
    case Notice::SsidEmpty:
      return page::kSsidEmpty;
    case Notice::SsidTooLong:
      return page::kSsidTooLong;
    case Notice::PasswordTooShort:
      return page::kPasswordTooShort;
    case Notice::PasswordTooLong:
      return page::kPasswordTooLong;
    case Notice::BadCharacters:
      return page::kBadCharacters;
    case Notice::ConfirmOpen:
      return page::kConfirmOpen;
    case Notice::SaveFailed:
      return page::kSaveFailed;
    case Notice::Expired:
      return page::kExpired;
    case Notice::None:
      break;
  }
  return nullptr;
}

// The page is plain HTML with a little CSS, no script and no outside file, because the phone has no internet on the setup network.
// The colors are set for both text and background, so a phone in dark mode does not mix them.
const char kHead[] =
    "<!doctype html><html lang=\"id\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta name=\"color-scheme\" content=\"light\">"
    "<style>body{font-family:sans-serif;margin:0;padding:16px;max-width:480px;background:#fff;color:#111}"
    "label{display:block;margin-top:16px}input{width:100%;box-sizing:border-box;padding:12px;font-size:16px;background:#fff;color:#111}"
    "button{margin-top:24px;padding:12px 16px;font-size:16px}small{display:block;margin-top:4px;color:#444}"
    ".n{margin-top:16px;padding:12px;border:2px solid #900;color:#900}</style><title>";

}  // namespace

Notice noticeForForm(FormResult result) {
  switch (result) {
    case FormResult::Ok:
      return Notice::None;
    case FormResult::SsidEmpty:
      return Notice::SsidEmpty;
    case FormResult::SsidTooLong:
      return Notice::SsidTooLong;
    case FormResult::PasswordTooShort:
      return Notice::PasswordTooShort;
    case FormResult::PasswordTooLong:
      return Notice::PasswordTooLong;
    case FormResult::BadCharacters:
      return Notice::BadCharacters;
  }
  return Notice::None;
}

SaveCheck decideSave(const char *ssid, const char *password, const char *confirmedSsid) {
  const FormResult form = checkWifiForm(ssid, password);
  if (form != FormResult::Ok) {
    return {SaveDecision::Invalid, form};
  }
  const bool open = password == nullptr || password[0] == '\0';
  const bool confirmed = confirmedSsid != nullptr && confirmedSsid[0] != '\0' && strcmp(confirmedSsid, ssid) == 0;
  if (open && !confirmed) {
    return {SaveDecision::AskOpenConfirm, FormResult::Ok};
  }
  return {SaveDecision::Save, FormResult::Ok};
}

bool escapeHtml(const char *in, char *out, size_t size) {
  if (size == 0) {
    return false;
  }
  size_t length = 0;
  for (const char *p = in; p != nullptr && *p != '\0'; p++) {
    const char *entity = nullptr;
    switch (*p) {
      case '&':
        entity = "&amp;";
        break;
      case '<':
        entity = "&lt;";
        break;
      case '>':
        entity = "&gt;";
        break;
      case '"':
        entity = "&quot;";
        break;
      case '\'':
        entity = "&#39;";
        break;
      default:
        break;
    }
    const size_t need = entity != nullptr ? strlen(entity) : 1;
    if (length + need + 1 > size) {
      out[0] = '\0';
      return false;
    }
    if (entity != nullptr) {
      memcpy(out + length, entity, need);
    } else {
      out[length] = *p;
    }
    length += need;
  }
  out[length] = '\0';
  return true;
}

size_t renderSetupPage(const PageModel &model, char *out, size_t size) {
  Writer w(out, size);
  w.add(kHead);
  w.addEscaped(page::kTitle);
  w.add("</title></head><body><h1>");
  w.addEscaped(page::kTitle);
  w.add("</h1><p>");
  w.addEscaped(page::kIntro);
  w.add("</p><p>");
  w.addEscaped(page::kBandHint);
  w.add(" ");
  w.addEscaped(page::kBandHintTwoNames);
  w.add("</p>");

  const char *notice = noticeText(model.notice);
  if (notice != nullptr) {
    w.add("<p class=\"n\" role=\"alert\">");
    w.addEscaped(notice);
    w.add("</p>");
  }

  w.add("<form method=\"post\" action=\"");
  w.add(kSavePath);
  w.add("\" autocomplete=\"off\"><label for=\"");
  w.add(kFieldSsid);
  w.add("\">");
  w.addEscaped(page::kSsidLabel);
  w.add("</label><input id=\"");
  w.add(kFieldSsid);
  w.add("\" name=\"");
  w.add(kFieldSsid);
  w.add("\" maxlength=\"");
  w.addNumber(kMaxSsidLength);
  w.add("\" autocapitalize=\"none\" autocorrect=\"off\" spellcheck=\"false\" required value=\"");
  // A name that is too long to be valid is not shown again, so the buffer for the escaped text stays small.
  if (model.ssid != nullptr && strnlen(model.ssid, kMaxSsidLength + 1) <= kMaxSsidLength) {
    w.addEscaped(model.ssid);
  }
  w.add("\"><label for=\"");
  w.add(kFieldPassword);
  w.add("\">");
  w.addEscaped(page::kPasswordLabel);
  w.add("</label><input id=\"");
  w.add(kFieldPassword);
  w.add("\" name=\"");
  w.add(kFieldPassword);
  w.add("\" type=\"password\" maxlength=\"");
  w.addNumber(kMaxPasswordLength);
  w.add("\" autocomplete=\"off\" autocapitalize=\"none\" autocorrect=\"off\" aria-describedby=\"pwhint\"><small id=\"pwhint\">");
  w.addEscaped(page::kPasswordHint);
  w.add("</small>");
  if (model.openConfirmed) {
    // The hidden field holds the name that is confirmed, and the server compares it with the name in the form.
    w.add("<input type=\"hidden\" name=\"");
    w.add(kFieldOpenConfirmed);
    w.add("\" value=\"");
    if (model.ssid != nullptr && strnlen(model.ssid, kMaxSsidLength + 1) <= kMaxSsidLength) {
      w.addEscaped(model.ssid);
    }
    w.add("\">");
  }
  w.add("<button type=\"submit\">");
  w.addEscaped(page::kSaveButton);
  w.add("</button></form></body></html>");
  return w.finish();
}

size_t renderSavedPage(char *out, size_t size) {
  Writer w(out, size);
  w.add(kHead);
  w.addEscaped(page::kSavedTitle);
  w.add("</title></head><body><h1>");
  w.addEscaped(page::kSavedTitle);
  w.add("</h1><p>");
  w.addEscaped(page::kSavedBody);
  w.add("</p></body></html>");
  return w.finish();
}

}  // namespace wattlink
