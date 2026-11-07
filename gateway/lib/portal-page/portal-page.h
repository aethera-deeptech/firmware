#pragma once

#include <stddef.h>

#include "wifi-form.h"

namespace wattlink {

enum class Notice {
  None,
  SsidEmpty,
  SsidTooLong,
  PasswordTooShort,
  PasswordTooLong,
  BadCharacters,
  ConfirmOpen,  // the password is empty, and the owner has to press Save once more
  SaveFailed,
  Expired,
};

Notice noticeForForm(FormResult result);

enum class SaveDecision {
  Save,
  AskOpenConfirm,  // an empty password is allowed only after the owner confirmed it
  Invalid,
};

struct SaveCheck {
  SaveDecision decision;
  FormResult form;  // the reason when the decision is Invalid, Ok otherwise
};

// Decides what the hub does with the form. An invalid form is refused. An empty password is an open network, and the first time the
// owner submits it the page asks again, because the hub would then join the network with no encryption. openConfirmed is true on the
// second submit.
SaveCheck decideSave(const char *ssid, const char *password, bool openConfirmed);

// What the page shows. The password is never part of it, so the page can never send a password back to the phone.
struct PageModel {
  Notice notice;
  const char *ssid;    // the name that the owner typed, shown again in the field. May be null.
  bool openConfirmed;  // adds the hidden field that confirms an open network
};

// The names of the form fields, shared with the code that reads the form.
constexpr const char *kFieldSsid = "ssid";
constexpr const char *kFieldPassword = "password";
constexpr const char *kFieldOpenConfirmed = "open_ok";
constexpr const char *kSavePath = "/save";

// HTML escaping for text and for the value of an attribute: & < > " ' become entities. Returns false when out is too small, and then out
// holds an empty text. A null input is an empty text.
bool escapeHtml(const char *in, char *out, size_t size);

// Writes the setup page, and returns its length. Returns 0, with an empty text, when out is too small. The text is UTF-8.
size_t renderSetupPage(const PageModel &model, char *out, size_t size);

// The page that follows a saved form.
size_t renderSavedPage(char *out, size_t size);

}  // namespace wattlink
