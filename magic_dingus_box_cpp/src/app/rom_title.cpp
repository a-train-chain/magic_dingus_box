#include "rom_title.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace app {

namespace {

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

// Regions and dump flags that No-Intro / GoodTools append. Anything not on
// this list is assumed to be part of the actual game name.
const std::set<std::string>& known_tags() {
    static const std::set<std::string> tags = {
        // Regions
        "usa", "europe", "japan", "world", "asia", "australia", "korea",
        "brazil", "canada", "china", "france", "germany", "italy", "spain",
        "netherlands", "sweden", "norway", "denmark", "finland", "russia",
        "taiwan", "hong kong", "uk", "united kingdom", "japan, usa",
        "usa, europe", "japan, europe", "unknown",
        // Dump / build flags
        "beta", "proto", "prototype", "demo", "sample", "unl", "unlicensed",
        "alt", "pirate", "kiosk", "gamecube", "lodgenet", "virtual console",
        "wii", "wii u", "switch online", "e-reader", "aftermarket",
        "disc 1", "disc 2", "disc 3", "disc 4",
    };
    return tags;
}

// Language-code lists such as "En,Fr,De" or "En,Ja,Fr,De,Es".
bool is_language_list(const std::string& body) {
    static const std::set<std::string> codes = {
        "en", "ja", "jp", "fr", "de", "es", "it", "nl", "pt", "sv", "no",
        "da", "fi", "zh", "ko", "pl", "ru", "cs", "hu", "el", "tr", "ar",
        "he", "ca", "gl", "eu",
    };
    if (body.find(',') == std::string::npos && body.size() != 2) {
        return false;
    }
    std::stringstream ss(body);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (codes.count(lowercase(trim(part))) == 0) {
            return false;
        }
    }
    return true;
}

// "Rev 1", "Rev A", "v1.1", "PRG0", "1.2"
bool is_revision(const std::string& body) {
    const std::string low = lowercase(body);
    if (low.rfind("rev ", 0) == 0 || low.rfind("prg", 0) == 0) {
        return true;
    }
    if (low.size() > 1 && low[0] == 'v' &&
        std::isdigit(static_cast<unsigned char>(low[1]))) {
        return true;
    }
    // A bare version number like "1.1". The dot is REQUIRED: without it
    // this would also match a year, and "Donkey Kong (1981)" would lose
    // part of its actual name.
    return !low.empty() && low.find('.') != std::string::npos &&
           low.find_first_not_of("0123456789.") == std::string::npos;
}

bool is_strippable_tag(const std::string& body) {
    const std::string low = lowercase(trim(body));
    return known_tags().count(low) > 0 || is_language_list(body) ||
           is_revision(body);
}

}  // namespace

std::string title_from_rom_path(const std::string& rom_path) {
    // 1. Filename without directory.
    const auto slash = rom_path.find_last_of("/\\");
    std::string name = (slash == std::string::npos)
                           ? rom_path
                           : rom_path.substr(slash + 1);

    // 2. Drop the extension. Guard against a leading dot so a name that is
    //    only an extension doesn't become empty.
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }

    // 3. Peel recognized trailing (tags) and [flags], repeatedly — real
    //    filenames stack several, e.g. "... (USA) (En,Fr) (Rev 1)".
    bool stripped = true;
    while (stripped) {
        stripped = false;
        name = trim(name);
        if (name.size() < 2) {
            break;
        }
        if (name.back() == ']') {
            const auto open = name.find_last_of('[');
            if (open != std::string::npos) {
                name = name.substr(0, open);
                stripped = true;
                continue;
            }
        }
        if (name.back() == ')') {
            const auto open = name.find_last_of('(');
            if (open != std::string::npos &&
                is_strippable_tag(
                    name.substr(open + 1, name.size() - open - 2))) {
                name = name.substr(0, open);
                stripped = true;
            }
        }
    }
    name = trim(name);

    // 4. "Legend of Zelda, The" -> "The Legend of Zelda". The article sits
    //    either at the very end or immediately before a " - " subtitle.
    for (const char* article : {"The", "A", "An"}) {
        const std::string suffix = std::string(", ") + article;
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) ==
                0) {
            name = article + std::string(" ") +
                   name.substr(0, name.size() - suffix.size());
            break;
        }
        const std::string infix = suffix + " - ";
        const auto at = name.find(infix);
        if (at != std::string::npos) {
            name = article + std::string(" ") + name.substr(0, at) + " - " +
                   name.substr(at + infix.size());
            break;
        }
    }

    // 5. " - " reads as a subtitle separator in these naming conventions.
    //    Only the spaced form: hyphens inside a word (F-Zero, Banjo-Tooie)
    //    must survive.
    const std::string sep = " - ";
    for (auto at = name.find(sep); at != std::string::npos;
         at = name.find(sep, at + 2)) {
        name.replace(at, sep.size(), ": ");
    }

    return trim(name);
}

}  // namespace app
