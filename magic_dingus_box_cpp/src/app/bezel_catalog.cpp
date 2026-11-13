#include "app/bezel_catalog.h"

#include <json/json.h>

#include <memory>

namespace app {

namespace {

// jsoncpp's asString() throws on arrays and objects; those read as "".
std::string string_field(const Json::Value& entry, const char* key) {
    const Json::Value& v = entry[key];
    return v.isConvertibleTo(Json::stringValue) ? v.asString() : std::string();
}

// The opening, when it is four integers naming a non-empty rect inside the
// bezel image. Leaves `bezel` untouched otherwise.
void read_opening(const Json::Value& entry, BezelInfo& bezel) {
    const Json::Value& o = entry["opening"];
    if (!o.isObject()) return;
    for (const char* key : {"x", "y", "w", "h"}) {
        if (!o[key].isInt()) return;
    }
    const int x = o["x"].asInt();
    const int y = o["y"].asInt();
    const int w = o["w"].asInt();
    const int h = o["h"].asInt();
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x > kBezelImageWidth - w || y > kBezelImageHeight - h) return;
    bezel.has_opening = true;
    bezel.opening_x = x;
    bezel.opening_y = y;
    bezel.opening_w = w;
    bezel.opening_h = h;
}

}  // namespace

std::vector<BezelInfo> parse_bezel_catalog(const std::string& json_text) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(json_text.data(), json_text.data() + json_text.size(),
                       &root, &errors)) {
        return {};
    }
    if (!root.isObject() || !root["bezels"].isArray()) return {};

    std::vector<BezelInfo> bezels;
    for (const Json::Value& entry : root["bezels"]) {
        if (!entry.isObject()) continue;
        BezelInfo bezel;
        bezel.id = string_field(entry, "id");
        bezel.name = string_field(entry, "name");
        bezel.file = string_field(entry, "file");  // null -> "" (procedural)
        bezel.description = string_field(entry, "description");
        read_opening(entry, bezel);
        if (!bezel.name.empty()) bezels.push_back(bezel);
    }
    return bezels;
}

}  // namespace app
