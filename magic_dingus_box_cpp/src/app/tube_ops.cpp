#include "app/tube_ops.h"

#include <json/json.h>

#include <exception>
#include <memory>

#include "tube/tube_setting.h"

namespace app {
namespace {

// A JSON integer by its type: jsoncpp's isInt()/isIntegral() also accept 20.0.
bool is_json_integer(const Json::Value& v) {
    return v.type() == Json::intValue || v.type() == Json::uintValue;
}

// The value as an unsigned integer, or nullopt when it is not a JSON integer or
// is negative. Read in the value's own width, so nothing overflows.
std::optional<std::uint64_t> json_unsigned(const Json::Value& v) {
    if (v.type() == Json::uintValue) return v.asLargestUInt();
    if (v.type() == Json::intValue && v.asLargestInt() >= 0) return static_cast<std::uint64_t>(v.asLargestInt());
    return std::nullopt;
}

const Json::Value& member(const Json::Value& object, const char* field) {
    return object.isMember(field) ? object[field] : Json::Value::nullSingleton();
}

std::string at(Json::ArrayIndex i) { return "ops[" + std::to_string(i) + "]"; }

// The op's `field` as a string, or nullopt when absent or not a string.
std::optional<std::string> string_field(const Json::Value& op, const char* field) {
    if (!op.isMember(field)) return std::nullopt;
    const Json::Value& v = op[field];
    if (!v.isString()) return std::nullopt;
    return v.asString();
}

// Parses one op into `out`; "" on success, else the reason (without the index).
std::string parse_op(const Json::Value& in, std::uint64_t previous_seq, TubeOp& out) {
    if (!in.isObject()) return "is not an object";

    const auto seq = json_unsigned(member(in, "seq"));
    if (!seq || *seq < 1) return "seq is not an integer >= 1";
    out.seq = *seq;
    if (out.seq <= previous_seq) return "seq is not greater than the previous op's";

    const auto name = string_field(in, "op");
    if (!name) return "op is missing or not a string";
    if (*name == "row") {
        out.kind = TubeOpKind::Row;
        const auto key = string_field(in, "key");
        const auto effect = key ? tube::parse_effect(*key) : std::nullopt;
        if (!effect) return "row key is not a known row";
        out.effect = *effect;
        const Json::Value& value = member(in, "value");
        if (!is_json_integer(value)) return "row value is not an integer";
        // Read in the value's own width before anything narrows it.
        const bool in_range = value.type() == Json::uintValue
                                  ? value.asLargestUInt() <= static_cast<Json::LargestUInt>(tube::kRowMax)
                                  : tube::is_row_value(value.asLargestInt());
        if (!in_range) return "row value is outside 0..100";
        out.value = static_cast<int>(value.asLargestInt());  // 0..100 here
    } else if (*name == "mask") {
        out.kind = TubeOpKind::Mask;
        const auto word = string_field(in, "mask");
        const auto mask = word ? tube::parse_mask(*word) : std::nullopt;
        if (!mask) return "mask is not a known mask";
        out.mask = *mask;
    } else if (*name == "look") {
        out.kind = TubeOpKind::Look;
        const auto word = string_field(in, "look");
        const auto look = word ? tube::parse_look(*word) : std::nullopt;
        if (!look) return "look is not a known look";
        out.look = *look;
    } else if (*name == "reset") {
        out.kind = TubeOpKind::Reset;
    } else if (*name == "engine") {
        out.kind = TubeOpKind::Engine;
        const auto word = string_field(in, "engine");
        const auto engine = word ? parse_crt_engine(*word) : std::nullopt;
        if (!engine) return "engine is not a known engine";
        out.engine = *engine;
    } else if (*name == "degauss") {
        out.kind = TubeOpKind::Degauss;
    } else {
        return "op is not one of row|mask|look|reset|engine|degauss";
    }
    return "";
}

TubeOpsParse refuse(std::string why) {
    TubeOpsParse out;
    out.error = std::move(why);
    return out;
}

}  // namespace

TubeOpsParse parse_tube_ops(std::string_view text) {
    if (text.size() > kTubeOpsMaxBytes)
        return refuse("larger than " + std::to_string(kTubeOpsMaxBytes) + " bytes");
    try {
        Json::CharReaderBuilder builder;
        // Strict: an object or array root only, no comments, no trailing text,
        // no duplicate keys, bounded nesting (jsoncpp throws past the limit).
        Json::CharReaderBuilder::strictMode(&builder.settings_);
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value root;
        std::string errors;
        if (!reader->parse(text.data(), text.data() + text.size(), &root, &errors))
            return refuse("not JSON");
        if (!root.isObject()) return refuse("not a JSON object");

        if (json_unsigned(member(root, "version")) != std::optional<std::uint64_t>{1})
            return refuse("version is not 1");

        const Json::Value& ops = member(root, "ops");
        if (!ops.isArray()) return refuse("ops is not an array");

        std::vector<TubeOp> out;
        out.reserve(ops.size());
        std::uint64_t previous_seq = 0;
        for (Json::ArrayIndex i = 0; i < ops.size(); ++i) {
            TubeOp op;
            const std::string why = parse_op(ops[i], previous_seq, op);
            if (!why.empty()) return refuse(at(i) + ": " + why);
            previous_seq = op.seq;
            out.push_back(op);
        }
        TubeOpsParse result;
        result.ops = std::move(out);
        return result;
    } catch (const std::exception& e) {
        return refuse(std::string("not readable JSON (") + e.what() + ")");
    } catch (...) {
        return refuse("not readable JSON");
    }
}

void adopt_tube_ops(const std::vector<TubeOp>* found, TubeLiveState& live) noexcept {
    live.adopted = true;
    if (found && !found->empty()) live.handled_seq = found->back().seq;
}

TubeOpsResult apply_tube_ops(const std::vector<TubeOp>& ops, AppState::DisplaySettings& display,
                             TubeLiveState& live, bool offered) {
    const CrtEngine engine_before = display.crt_engine;
    const tube::TubeSetting tube_before = display.tube;
    TubeOpsResult result;
    for (const TubeOp& op : ops) {
        if (op.seq <= live.handled_seq) continue;
        live.handled_seq = op.seq;
        if (!offered) {
            ++result.skipped;
            continue;
        }
        switch (op.kind) {
            case TubeOpKind::Row:
                if (!tube::set_row(display.tube, op.effect, op.value)) {
                    ++result.skipped;
                    continue;
                }
                break;
            case TubeOpKind::Mask: tube::set_mask(display.tube, op.mask); break;
            case TubeOpKind::Look: tube::select_look(display.tube, op.look); break;
            case TubeOpKind::Reset: tube::reset_to_look(display.tube); break;
            case TubeOpKind::Engine: display.set_crt_engine(op.engine); break;
            case TubeOpKind::Degauss: ++live.degauss_requests; break;
        }
        ++result.applied;
    }
    result.changed = display.crt_engine != engine_before || display.tube != tube_before;
    return result;
}

}  // namespace app
