#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "app/app_state.h"
#include "app/controller.h"
#include "ui/virtual_keyboard.h"

namespace fs = std::filesystem;

namespace {
// Helper: write JSON-Lines events into the queue file.
void write_queue(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::trunc);
    f << content;
}
}

TEST_CASE("poll_text_input_queue idle fast path", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_idle.jsonl";
    fs::remove(tmp);

    ui::VirtualKeyboard kb;
    kb.open("hello", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    // No file → no-op, no exception, buffer unchanged.
    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "hello");
}

TEST_CASE("poll_text_input_queue type_char appends", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_typechar.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"s\",\"seq\":1}\n"
        "{\"t\":\"type_char\",\"c\":\"h\",\"seq\":2}\n"
        "{\"t\":\"type_char\",\"c\":\"a\",\"seq\":3}\n");

    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "sha");
    // File should be truncated after drain.
    REQUIRE(fs::file_size(tmp) == 0);
}

TEST_CASE("poll_text_input_queue dispatches backspace and enter", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_special.jsonl";
    write_queue(tmp,
        "{\"t\":\"key_special\",\"k\":\"backspace\",\"seq\":1}\n"
        "{\"t\":\"key_special\",\"k\":\"enter\",\"seq\":2}\n");

    std::string captured;
    ui::VirtualKeyboard kb;
    kb.open("hello", "T",
            [&](const std::string& s){ captured = s; },
            nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "hell");          // backspace ate the 'o'
    REQUIRE(captured == "hell");                // commit fired on_enter
}

TEST_CASE("poll_text_input_queue clear wipes buffer", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_clear.jsonl";
    write_queue(tmp, "{\"t\":\"clear\",\"seq\":1}\n");

    ui::VirtualKeyboard kb;
    kb.open("password", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "");
}

TEST_CASE("poll_text_input_queue drops events when no receiver", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_dropped.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"x\",\"seq\":1}\n");

    app::AppState state;
    state.active_text_keyboard = nullptr;       // no receiver
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    REQUIRE_NOTHROW(c.poll_text_input_queue(state));
    REQUIRE(fs::file_size(tmp) == 0);            // still truncated
}

TEST_CASE("poll_text_input_queue skips malformed lines", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_malformed.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"a\",\"seq\":1}\n"
        "this is not json\n"
        "{\"t\":\"type_char\",\"c\":\"b\",\"seq\":2}\n");

    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "ab");
    REQUIRE(fs::file_size(tmp) == 0);
}
