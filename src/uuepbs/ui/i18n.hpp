// UUEPBS - optional UI translation.
//
// Every user-visible string is passed through ui::tr(). English is the built-in language; the
// Chinese table below translates the main interface. Strings that are not in the table fall
// back to English, so adding a translation never breaks the UI.
#pragma once

#include <string_view>

namespace uuepbs::ui
{
    enum class Language
    {
        English = 0,
        Chinese = 1,
    };

    void set_language(Language language);
    Language language();
    // "en", "english", "zh", "chinese", "cn", "auto" (system language; Chinese only for Chinese locales).
    void set_language_from_setting(std::string_view setting);
    const char* language_name(Language language);
    // Returns the translation for `text`, or `text` itself when there is none.
    const char* tr(const char* text);
} // namespace uuepbs::ui
