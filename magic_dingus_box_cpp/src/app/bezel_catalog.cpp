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
// bezel image. Leaves `bezel` untouched otherwise and returns why ("" when
// it was accepted, or when the entry has no opening at all).
std::string read_opening(const Json::Value& entry, BezelInfo& bezel) {
    if (!entry.isMember("opening")) return "";
    const Json::Value& o = entry["opening"];
    if (!o.isObject()) return "not an object";
    for (const char* key : {"x", "y", "w", "h"}) {
        if (!o[key].isInt()) {
            return std::string("\"") + key + "\" is missing or not an integer";
        }
    }
    const int x = o["x"].asInt();
    const int y = o["y"].asInt();
    const int w = o["w"].asInt();
    const int h = o["h"].asInt();
    if (x < 0 || y < 0 || w <= 0 || h <= 0) {
        return "negative x/y or an empty size";
    }
    if (x > kBezelImageWidth - w || y > kBezelImageHeight - h) {
        return "extends past the " + std::to_string(kBezelImageWidth) + "x" +
               std::to_string(kBezelImageHeight) + " image";
    }
    bezel.has_opening = true;
    bezel.opening_x = x;
    bezel.opening_y = y;
    bezel.opening_w = w;
    bezel.opening_h = h;
    return "";
}

}  // namespace

std::vector<BezelInfo> parse_bezel_catalog(const std::string& json_text,
                                           std::vector<std::string>* problems) {
    auto report = [problems](std::string line) {
        if (problems) problems->push_back(std::move(line));
    };

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(json_text.data(), json_text.data() + json_text.size(),
                       &root, &errors)) {
        // One log line: jsoncpp writes "* Line N, Column M\n  <message>\n"
        // per error; join its lines with "; " and drop the trailing newline.
        std::string joined;
        bool line_start = false;
        for (const char c : errors) {
            if (c == '\n') {
                line_start = true;
                continue;
            }
            if (line_start) {
                if (c == ' ') continue;  // the next line's indent
                joined += "; ";
                line_start = false;
            }
            joined += c;
        }
        report("not valid JSON: " + joined);
        return {};
    }
    if (!root.isObject() || !root["bezels"].isArray()) {
        report("no \"bezels\" array");
        return {};
    }

    std::vector<BezelInfo> bezels;
    for (const Json::Value& entry : root["bezels"]) {
        if (!entry.isObject()) continue;
        BezelInfo bezel;
        bezel.id = string_field(entry, "id");
        bezel.name = string_field(entry, "name");
        bezel.file = string_field(entry, "file");  // null -> "" (procedural)
        bezel.description = string_field(entry, "description");
        const std::string refused = read_opening(entry, bezel);
        if (bezel.name.empty()) continue;
        if (!refused.empty()) {
            report("bezel \"" + bezel.name + "\": opening ignored (" + refused +
                   ") — its video uses the whole 4:3 area");
        }
        bezels.push_back(bezel);
    }
    return bezels;
}

}  // namespace app
