#pragma once

#include <string>

namespace video {

class VideoPlayer {
public:
    virtual ~VideoPlayer() = default;

    virtual bool initialize(const std::string& hwdec = "no") = 0;
    virtual bool load_file(const std::string& path, double start = 0.0, double end = 0.0, bool loop = false) = 0;
    
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void toggle_pause() = 0;
    virtual void seek(double seconds) = 0;
    virtual void seek_absolute(double timestamp) = 0;
    virtual void stop() = 0;
    
    virtual bool is_playing() const = 0;
    virtual bool is_paused() const = 0;
    virtual double get_position() const = 0;
    virtual double get_duration() const = 0;
    
    virtual void set_volume(double volume) = 0;
    virtual double get_volume() const = 0;
    
    virtual void cleanup() = 0;
};

} // namespace video

