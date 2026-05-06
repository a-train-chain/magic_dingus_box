#include <catch2/catch_test_macros.hpp>
#include "ui/virtual_keyboard.h"

TEST_CASE("VirtualKeyboard::type_char appends to buffer", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    kb.open("", "Test", nullptr, nullptr);
    REQUIRE(kb.get_text() == "");

    kb.type_char('a');
    kb.type_char('b');
    kb.type_char('c');
    REQUIRE(kb.get_text() == "abc");
}

TEST_CASE("VirtualKeyboard::type_char ignored when not active", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    // Not opened — should be inactive
    REQUIRE_FALSE(kb.is_active());
    kb.type_char('x');
    REQUIRE(kb.get_text() == "");
}

TEST_CASE("VirtualKeyboard::clear_buffer wipes text", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    kb.open("hello", "Test", nullptr, nullptr);
    REQUIRE(kb.get_text() == "hello");

    kb.clear_buffer();
    REQUIRE(kb.get_text() == "");
}

TEST_CASE("VirtualKeyboard::commit fires on_enter when set", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    std::string captured;
    kb.open("password123", "Wi-Fi",
            [&](const std::string& t) { captured = t; },
            nullptr);

    kb.commit();
    REQUIRE(captured == "password123");
}

TEST_CASE("VirtualKeyboard::commit no-op when on_enter unset", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    kb.open("hello", "Search", nullptr, nullptr);
    // Should not crash; nothing to capture. Keyboard still closes
    // after commit() — same semantic as select()'s ENTER key.
    REQUIRE_NOTHROW(kb.commit());
    REQUIRE(kb.get_text() == "hello");
    REQUIRE_FALSE(kb.is_active());
}

TEST_CASE("VirtualKeyboard::commit closes keyboard like select-ENTER", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    kb.open("hello", "T", nullptr, nullptr);
    REQUIRE(kb.is_active());

    kb.commit();
    REQUIRE_FALSE(kb.is_active());
}
