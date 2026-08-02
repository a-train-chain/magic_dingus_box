#include "media_browser/ui/mb_filter_state.h"

#include <string>

namespace media_browser::ui {

namespace {

using DS = ::app::AppState::DisplaySettings;

struct GenreEntry { int tmdb_id; const char* display; };

// /genre/movie/list — moved verbatim from mb_filter_overlay.cpp. Bit order is
// ON-DISK STATE (genre_mask bits are persisted), so never reorder or insert.
constexpr GenreEntry kMovieGenres[] = {
    {28,    "Action"},    {12,    "Adventure"}, {16,    "Animation"}, {35,    "Comedy"},
    {80,    "Crime"},     {99,    "Doc"},        {18,    "Drama"},     {10751, "Family"},
    {14,    "Fantasy"},   {36,    "History"},    {27,    "Horror"},    {10402, "Music"},
    {9648,  "Mystery"},   {10749, "Romance"},    {878,   "Sci-Fi"},    {10770, "TV Movie"},
    {53,    "Thriller"},  {10752, "War"},
};
// /genre/tv/list — all 16, TMDB's own order. A DIFFERENT id space: 10759 and
// 10765 exist only here, and 11 movie ids are invalid for TV. Display strings
// are shortened to fit the overlay's right-aligned value column.
constexpr GenreEntry kTvGenres[] = {
    {10759, "Action & Adv"}, {16,    "Animation"}, {35,    "Comedy"},   {80,    "Crime"},
    {99,    "Doc"},          {18,    "Drama"},     {10751, "Family"},   {10762, "Kids"},
    {9648,  "Mystery"},      {10763, "News"},      {10764, "Reality"},  {10765, "Sci-Fi & Fant"},
    {10766, "Soap"},         {10767, "Talk"},      {10768, "War & Pol"},{37,    "Western"},
};

constexpr int kNumMovieGenres =
    static_cast<int>(sizeof(kMovieGenres) / sizeof(kMovieGenres[0]));
constexpr int kNumTvGenres =
    static_cast<int>(sizeof(kTvGenres) / sizeof(kTvGenres[0]));

const GenreEntry* catalog(MbMode mode) {
    return (mode == MbMode::Tv) ? kTvGenres : kMovieGenres;
}

const char* kMovieRuntimeLabels[] = {"Any", "<90m", "90-120m", "2-3hr", "3hr+"};
const char* kTvRuntimeLabels[]    = {"Any", "<30m", "30-45m", "45-60m", "60m+"};

// FilterTabKind -> the persisted chart slot. ForYou has none; callers check
// for it before reaching here.
MbChartTab chart_tab_of(FilterTabKind tab) {
    return (tab == FilterTabKind::Popular) ? MbChartTab::Popular
                                           : MbChartTab::TopRated;
}

}  // namespace

const std::vector<int>& filter_genre_ids(MbMode mode) {
    static const std::vector<int> movie_ids = []() {
        std::vector<int> v;
        v.reserve(kNumMovieGenres);
        for (int i = 0; i < kNumMovieGenres; ++i) v.push_back(kMovieGenres[i].tmdb_id);
        return v;
    }();
    static const std::vector<int> tv_ids = []() {
        std::vector<int> v;
        v.reserve(kNumTvGenres);
        for (int i = 0; i < kNumTvGenres; ++i) v.push_back(kTvGenres[i].tmdb_id);
        return v;
    }();
    return (mode == MbMode::Tv) ? tv_ids : movie_ids;
}

int filter_genre_count(MbMode mode) {
    return (mode == MbMode::Tv) ? kNumTvGenres : kNumMovieGenres;
}

const char* filter_genre_display(MbMode mode, uint32_t mask) {
    if (mask == 0) return "All";
    const int bit = __builtin_ctz(mask);
    if (bit < filter_genre_count(mode)) return catalog(mode)[bit].display;
    return "?";
}

const char* const* filter_runtime_labels(MbMode mode) {
    return (mode == MbMode::Tv) ? kTvRuntimeLabels : kMovieRuntimeLabels;
}

const char* mode_row_value_label(MbMode mode) {
    return (mode == MbMode::Tv) ? "TV" : "MOVIES";
}

FilterState read_filter_state(const DS& s, MbMode mode, FilterTabKind tab) {
    if (tab == FilterTabKind::ForYou) return FilterState{};
    const DS::MbFilterSet& f = s.mb_filter(mode, chart_tab_of(tab));
    FilterState fs;
    fs.genre_mask = f.genre_mask;
    fs.decade     = static_cast<int>(f.decade);
    fs.min_rating = static_cast<int>(f.min_rating);
    fs.runtime    = static_cast<int>(f.runtime);
    fs.language   = static_cast<int>(f.language);
    fs.sort       = static_cast<int>(f.sort);
    return fs;
}

void write_filter_state(DS& s, MbMode mode, FilterTabKind tab,
                        const FilterState& fs) {
    if (tab == FilterTabKind::ForYou) return;   // no persisted state, either mode
    DS::MbFilterSet& f = s.mb_filter(mode, chart_tab_of(tab));
    f.genre_mask = fs.genre_mask;
    f.decade     = static_cast<DS::MbDecade>(fs.decade);
    f.min_rating = static_cast<DS::MbMinRating>(fs.min_rating);
    f.runtime    = static_cast<DS::MbRuntime>(fs.runtime);
    f.language   = static_cast<DS::MbLanguage>(fs.language);
    f.sort       = static_cast<DS::MbDiscoverSort>(fs.sort);
}

FilterState apply_mode_toggle(DS& s, MbMode outgoing, MbMode incoming,
                              FilterTabKind tab, const FilterState& staged) {
    write_filter_state(s, outgoing, tab, staged);
    s.mb_mode = incoming;
    return read_filter_state(s, incoming, tab);
}

bool any_filter_active(const FilterState& fs, FilterTabKind tab) {
    if (fs.genre_mask != 0) return true;
    if (fs.decade != 0)     return true;
    if (fs.min_rating != 0) return true;
    if (fs.runtime != 0)    return true;
    if (fs.language != 0)   return true;
    if (tab == FilterTabKind::Popular &&
        fs.sort != static_cast<int>(DS::MbDiscoverSort::Popularity)) return true;
    if (tab == FilterTabKind::TopRated &&
        fs.sort != static_cast<int>(DS::MbDiscoverSort::TopRated))   return true;
    return false;
}

::media_browser::DiscoverFilter build_discover_filter(const FilterState& fs,
                                                      FilterTabKind tab) {
    ::media_browser::DiscoverFilter df;

    const auto& gids = filter_genre_ids(MbMode::Movies);
    for (int i = 0; i < static_cast<int>(gids.size()); ++i) {
        if ((fs.genre_mask & (1u << i)) != 0) df.genre_ids.push_back(gids[i]);
    }

    auto set_year_range = [&](int start, int end) {
        df.primary_release_year_gte = start;
        df.primary_release_year_lte = end;
    };
    switch (static_cast<DS::MbDecade>(fs.decade)) {
        case DS::MbDecade::D2020s:  set_year_range(2020, 2029); break;
        case DS::MbDecade::D2010s:  set_year_range(2010, 2019); break;
        case DS::MbDecade::D2000s:  set_year_range(2000, 2009); break;
        case DS::MbDecade::D1990s:  set_year_range(1990, 1999); break;
        case DS::MbDecade::D1980s:  set_year_range(1980, 1989); break;
        case DS::MbDecade::D1970s:  set_year_range(1970, 1979); break;
        case DS::MbDecade::Classic: df.primary_release_year_lte = 1969; break;
        case DS::MbDecade::Any:     break;
    }

    const auto rat = static_cast<DS::MbMinRating>(fs.min_rating);
    if (rat == DS::MbMinRating::Six)   df.vote_average_gte = 6.0f;
    if (rat == DS::MbMinRating::Seven) df.vote_average_gte = 7.0f;
    if (rat == DS::MbMinRating::Eight) df.vote_average_gte = 8.0f;

    switch (static_cast<DS::MbRuntime>(fs.runtime)) {
        case DS::MbRuntime::Under90:      df.with_runtime_lte = 89; break;
        case DS::MbRuntime::Range90To120: df.with_runtime_gte = 90;
                                          df.with_runtime_lte = 120; break;
        case DS::MbRuntime::Range2To3Hr:  df.with_runtime_gte = 121;
                                          df.with_runtime_lte = 180; break;
        case DS::MbRuntime::Over3Hr:      df.with_runtime_gte = 181; break;
        case DS::MbRuntime::Any:          break;
    }

    auto set_lang = [&](const char* code) { df.with_original_language = std::string{code}; };
    switch (static_cast<DS::MbLanguage>(fs.language)) {
        case DS::MbLanguage::English:  set_lang("en"); break;
        case DS::MbLanguage::Japanese: set_lang("ja"); break;
        case DS::MbLanguage::French:   set_lang("fr"); break;
        case DS::MbLanguage::Spanish:  set_lang("es"); break;
        case DS::MbLanguage::Korean:   set_lang("ko"); break;
        case DS::MbLanguage::Italian:  set_lang("it"); break;
        case DS::MbLanguage::German:   set_lang("de"); break;
        case DS::MbLanguage::Hindi:    set_lang("hi"); break;
        case DS::MbLanguage::Mandarin: set_lang("zh"); break;
        case DS::MbLanguage::Any:      break;
    }

    switch (static_cast<DS::MbDiscoverSort>(fs.sort)) {
        case DS::MbDiscoverSort::Popularity:    df.sort_by = "popularity.desc"; break;
        case DS::MbDiscoverSort::TopRated:      df.sort_by = "vote_average.desc"; break;
        case DS::MbDiscoverSort::MostVoted:     df.sort_by = "vote_count.desc"; break;
        case DS::MbDiscoverSort::RecentRelease: df.sort_by = "primary_release_date.desc"; break;
    }
    df.vote_count_gte = (tab == FilterTabKind::Popular) ? 200 : 300;

    return df;
}

::media_browser::TvDiscoverFilter build_tv_discover_filter(const FilterState& fs,
                                                           FilterTabKind tab) {
    ::media_browser::TvDiscoverFilter tf;

    const auto& gids = filter_genre_ids(MbMode::Tv);
    for (int i = 0; i < static_cast<int>(gids.size()); ++i) {
        if ((fs.genre_mask & (1u << i)) != 0) tf.genre_ids.push_back(gids[i]);
    }

    // first_air_date.* = the SERIES premiere. air_date.* would match any
    // episode's date and let a 1960s show through a "2020s" filter.
    auto set_year_range = [&](int start, int end) {
        tf.first_air_date_year_gte = start;
        tf.first_air_date_year_lte = end;
    };
    switch (static_cast<DS::MbDecade>(fs.decade)) {
        case DS::MbDecade::D2020s:  set_year_range(2020, 2029); break;
        case DS::MbDecade::D2010s:  set_year_range(2010, 2019); break;
        case DS::MbDecade::D2000s:  set_year_range(2000, 2009); break;
        case DS::MbDecade::D1990s:  set_year_range(1990, 1999); break;
        case DS::MbDecade::D1980s:  set_year_range(1980, 1989); break;
        case DS::MbDecade::D1970s:  set_year_range(1970, 1979); break;
        case DS::MbDecade::Classic: tf.first_air_date_year_lte = 1969; break;
        case DS::MbDecade::Any:     break;
    }

    const auto rat = static_cast<DS::MbMinRating>(fs.min_rating);
    if (rat == DS::MbMinRating::Six)   tf.vote_average_gte = 6.0f;
    if (rat == DS::MbMinRating::Seven) tf.vote_average_gte = 7.0f;
    if (rat == DS::MbMinRating::Eight) tf.vote_average_gte = 8.0f;

    // PER-EPISODE minutes — the movie bands would match nothing.
    switch (static_cast<DS::MbRuntime>(fs.runtime)) {
        case DS::MbRuntime::Under90:      tf.with_runtime_lte = 29; break;
        case DS::MbRuntime::Range90To120: tf.with_runtime_gte = 30;
                                          tf.with_runtime_lte = 45; break;
        case DS::MbRuntime::Range2To3Hr:  tf.with_runtime_gte = 46;
                                          tf.with_runtime_lte = 60; break;
        case DS::MbRuntime::Over3Hr:      tf.with_runtime_gte = 61; break;
        case DS::MbRuntime::Any:          break;
    }

    auto set_lang = [&](const char* code) { tf.with_original_language = std::string{code}; };
    switch (static_cast<DS::MbLanguage>(fs.language)) {
        case DS::MbLanguage::English:  set_lang("en"); break;
        case DS::MbLanguage::Japanese: set_lang("ja"); break;
        case DS::MbLanguage::French:   set_lang("fr"); break;
        case DS::MbLanguage::Spanish:  set_lang("es"); break;
        case DS::MbLanguage::Korean:   set_lang("ko"); break;
        case DS::MbLanguage::Italian:  set_lang("it"); break;
        case DS::MbLanguage::German:   set_lang("de"); break;
        case DS::MbLanguage::Hindi:    set_lang("hi"); break;
        case DS::MbLanguage::Mandarin: set_lang("zh"); break;
        case DS::MbLanguage::Any:      break;
    }

    switch (static_cast<DS::MbDiscoverSort>(fs.sort)) {
        case DS::MbDiscoverSort::Popularity:    tf.sort_by = "popularity.desc"; break;
        case DS::MbDiscoverSort::TopRated:      tf.sort_by = "vote_average.desc"; break;
        case DS::MbDiscoverSort::MostVoted:     tf.sort_by = "vote_count.desc"; break;
        // NOT primary_release_date.desc — that param does not exist for TV.
        case DS::MbDiscoverSort::RecentRelease: tf.sort_by = "first_air_date.desc"; break;
    }
    tf.vote_count_gte = (tab == FilterTabKind::Popular) ? kTvVoteCountPopular
                                                        : kTvVoteCountTopRated;

    return tf;
}

}  // namespace media_browser::ui
