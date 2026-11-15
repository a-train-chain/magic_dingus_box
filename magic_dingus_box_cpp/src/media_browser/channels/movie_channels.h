#pragma once

#include "app/app_state.h"
#include "movie_library.h"

namespace app {

bool codec_allowed(const std::vector<std::string>& allowed, const std::string& codec);
std::vector<Playlist> build_movie_playlists(const std::vector<LibraryMovie>& movies,
                                           const std::vector<std::string>& allowed_codecs);

}  // namespace app
