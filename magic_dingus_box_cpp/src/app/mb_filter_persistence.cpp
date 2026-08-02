#include "app/mb_filter_persistence.h"

#include <string>

namespace {

// --- moved verbatim from settings_persistence.cpp -------------------------
const char* mb_decade_to_string(app::AppState::DisplaySettings::MbDecade v) {
    using D = app::AppState::DisplaySettings::MbDecade;
    switch (v) {
        case D::D2020s:  return "2020s";
        case D::D2010s:  return "2010s";
        case D::D2000s:  return "2000s";
        case D::D1990s:  return "1990s";
        case D::D1980s:  return "1980s";
        case D::D1970s:  return "1970s";
        case D::Classic: return "classic";
        case D::Any:
        default:         return "any";
    }
}
app::AppState::DisplaySettings::MbDecade mb_decade_from_string(const std::string& s) {
    using D = app::AppState::DisplaySettings::MbDecade;
    if (s == "2020s")   return D::D2020s;
    if (s == "2010s")   return D::D2010s;
    if (s == "2000s")   return D::D2000s;
    if (s == "1990s")   return D::D1990s;
    if (s == "1980s")   return D::D1980s;
    if (s == "1970s")   return D::D1970s;
    if (s == "classic") return D::Classic;
    return D::Any;
}

const char* mb_min_rating_to_string(app::AppState::DisplaySettings::MbMinRating v) {
    using R = app::AppState::DisplaySettings::MbMinRating;
    switch (v) {
        case R::Six:   return "6";
        case R::Seven: return "7";
        case R::Eight: return "8";
        case R::Any:
        default:       return "any";
    }
}
app::AppState::DisplaySettings::MbMinRating mb_min_rating_from_string(const std::string& s) {
    using R = app::AppState::DisplaySettings::MbMinRating;
    if (s == "6") return R::Six;
    if (s == "7") return R::Seven;
    if (s == "8") return R::Eight;
    return R::Any;
}

const char* mb_runtime_to_string(app::AppState::DisplaySettings::MbRuntime v) {
    using T = app::AppState::DisplaySettings::MbRuntime;
    switch (v) {
        case T::Under90:      return "under90";
        case T::Range90To120: return "90to120";
        case T::Range2To3Hr:  return "2to3hr";
        case T::Over3Hr:      return "over3hr";
        case T::Any:
        default:              return "any";
    }
}
app::AppState::DisplaySettings::MbRuntime mb_runtime_from_string(const std::string& s) {
    using T = app::AppState::DisplaySettings::MbRuntime;
    if (s == "under90") return T::Under90;
    if (s == "90to120") return T::Range90To120;
    if (s == "2to3hr")  return T::Range2To3Hr;
    if (s == "over3hr") return T::Over3Hr;
    return T::Any;
}

const char* mb_language_to_string(app::AppState::DisplaySettings::MbLanguage v) {
    using L = app::AppState::DisplaySettings::MbLanguage;
    switch (v) {
        case L::English:  return "en";
        case L::Japanese: return "ja";
        case L::French:   return "fr";
        case L::Spanish:  return "es";
        case L::Korean:   return "ko";
        case L::Italian:  return "it";
        case L::German:   return "de";
        case L::Hindi:    return "hi";
        case L::Mandarin: return "zh";
        case L::Any:
        default:          return "any";
    }
}
app::AppState::DisplaySettings::MbLanguage mb_language_from_string(const std::string& s) {
    using L = app::AppState::DisplaySettings::MbLanguage;
    if (s == "en") return L::English;
    if (s == "ja") return L::Japanese;
    if (s == "fr") return L::French;
    if (s == "es") return L::Spanish;
    if (s == "ko") return L::Korean;
    if (s == "it") return L::Italian;
    if (s == "de") return L::German;
    if (s == "hi") return L::Hindi;
    if (s == "zh") return L::Mandarin;
    return L::Any;
}

const char* mb_discover_sort_to_string(app::AppState::DisplaySettings::MbDiscoverSort v) {
    using S = app::AppState::DisplaySettings::MbDiscoverSort;
    switch (v) {
        case S::TopRated:      return "top_rated";
        case S::MostVoted:     return "most_voted";
        case S::RecentRelease: return "recent_release";
        case S::Popularity:
        default:               return "popularity";
    }
}
app::AppState::DisplaySettings::MbDiscoverSort mb_discover_sort_from_string(const std::string& s) {
    using S = app::AppState::DisplaySettings::MbDiscoverSort;
    if (s == "top_rated")      return S::TopRated;
    if (s == "most_voted")     return S::MostVoted;
    if (s == "recent_release") return S::RecentRelease;
    return S::Popularity;
}
// --------------------------------------------------------------------------

using DS = app::AppState::DisplaySettings;

const char* mb_mode_to_string(DS::MbMode m) {
    return (m == DS::MbMode::Tv) ? "tv" : "movies";
}
DS::MbMode mb_mode_from_string(const std::string& s) {
    return (s == "tv") ? DS::MbMode::Tv : DS::MbMode::Movies;
}

// Key prefix for one (mode, tab) slot. These strings ARE the on-disk
// schema — the two movie prefixes must never change.
const char* prefix_for(DS::MbMode m, DS::MbChartTab t) {
    if (m == DS::MbMode::Movies) {
        return (t == DS::MbChartTab::Popular) ? "mb_popular_" : "mb_toprated_";
    }
    return (t == DS::MbChartTab::Popular) ? "mb_tv_popular_" : "mb_tv_toprated_";
}

void set_slot(Json::Value& display, const char* prefix, const DS::MbFilterSet& f) {
    const std::string p(prefix);
    display[p + "genre_mask"] = static_cast<Json::UInt>(f.genre_mask);
    display[p + "decade"]     = mb_decade_to_string(f.decade);
    display[p + "min_rating"] = mb_min_rating_to_string(f.min_rating);
    display[p + "runtime"]    = mb_runtime_to_string(f.runtime);
    display[p + "language"]   = mb_language_to_string(f.language);
    display[p + "sort"]       = mb_discover_sort_to_string(f.sort);
}

void get_slot(const Json::Value& display, const char* prefix, DS::MbFilterSet& f) {
    const std::string p(prefix);
    if (display.isMember(p + "genre_mask"))
        f.genre_mask = display[p + "genre_mask"].asUInt();
    if (display.isMember(p + "decade"))
        f.decade = mb_decade_from_string(display[p + "decade"].asString());
    if (display.isMember(p + "min_rating"))
        f.min_rating = mb_min_rating_from_string(display[p + "min_rating"].asString());
    if (display.isMember(p + "runtime"))
        f.runtime = mb_runtime_from_string(display[p + "runtime"].asString());
    if (display.isMember(p + "language"))
        f.language = mb_language_from_string(display[p + "language"].asString());
    if (display.isMember(p + "sort"))
        f.sort = mb_discover_sort_from_string(display[p + "sort"].asString());
}

}  // namespace

namespace app {

void mb_filters_to_json(const AppState::DisplaySettings& s,
                        Json::Value& display,
                        bool include_tv) {
    set_slot(display, prefix_for(DS::MbMode::Movies, DS::MbChartTab::Popular),
             s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::Popular));
    set_slot(display, prefix_for(DS::MbMode::Movies, DS::MbChartTab::TopRated),
             s.mb_filter(DS::MbMode::Movies, DS::MbChartTab::TopRated));
    if (!include_tv) return;
    display["mb_mode"] = mb_mode_to_string(s.mb_mode);
    set_slot(display, prefix_for(DS::MbMode::Tv, DS::MbChartTab::Popular),
             s.mb_filter(DS::MbMode::Tv, DS::MbChartTab::Popular));
    set_slot(display, prefix_for(DS::MbMode::Tv, DS::MbChartTab::TopRated),
             s.mb_filter(DS::MbMode::Tv, DS::MbChartTab::TopRated));
}

void mb_filters_from_json(const Json::Value& display,
                          AppState::DisplaySettings& s) {
    s.mb_mode = mb_mode_from_string(
        display.get("mb_mode", "movies").asString());
    for (auto mode : {DS::MbMode::Movies, DS::MbMode::Tv}) {
        for (auto tab : {DS::MbChartTab::Popular, DS::MbChartTab::TopRated}) {
            get_slot(display, prefix_for(mode, tab), s.mb_filter(mode, tab));
        }
    }
}

}  // namespace app
