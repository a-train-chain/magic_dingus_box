#include "virtual_keyboard.h"
#include <algorithm>
#include <iostream>

namespace ui {

static const std::vector<std::vector<std::string>> LAYOUT_LOWER = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", "-"},
    {"z", "x", "c", "v", "b", "n", "m", "_", ".", "@"},
    {"CAPS", "SYMB", "SPACE", "BACK", "ENTER", "CANCEL"}
};

static const std::vector<std::vector<std::string>> LAYOUT_UPPER = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L", "-"},
    {"Z", "X", "C", "V", "B", "N", "M", "_", ".", "@"},
    {"caps", "SYMB", "SPACE", "BACK", "ENTER", "CANCEL"}
};

static const std::vector<std::vector<std::string>> LAYOUT_SYMBOLS = {
    {"!", "\"", "#", "$", "%", "&", "'", "(", ")", "+"},
    {"~", "`", ":", ";", "/", "\\", "|", "{", "}", "?"},
    {"<", ">", "[", "]", "=", "*", "^", ",", ".", "@"},
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"CAPS", "symb", "SPACE", "BACK", "ENTER", "CANCEL"}
};

VirtualKeyboard::VirtualKeyboard()
    : active_(false)
    , selected_row_(0)
    , selected_col_(0)
    , caps_lock_(false)
    , symbols_mode_(false)
{
}

void VirtualKeyboard::open(const std::string& initial_text, const std::string& title, 
                          OnEnterCallback on_enter, OnCancelCallback on_cancel) {
    active_ = true;
    text_buffer_ = initial_text;
    title_ = title;
    on_enter_ = on_enter;
    on_cancel_ = on_cancel;
    
    selected_row_ = 1; // Start on Q
    selected_col_ = 0;
    caps_lock_ = false;
    symbols_mode_ = false;
}

void VirtualKeyboard::close() {
    active_ = false;
    if (on_cancel_) on_cancel_();
}

void VirtualKeyboard::navigate_up() {
    if (!active_) return;
    selected_row_--;
    if (selected_row_ < 0) selected_row_ = (int)get_layout().size() - 1;
    
    // Clamp col if row length differs (though currently they are uniform mostly, except last row)
    int row_len = (int)get_layout()[selected_row_].size();
    if (selected_col_ >= row_len) selected_col_ = row_len - 1;
}

void VirtualKeyboard::navigate_down() {
    if (!active_) return;
    selected_row_++;
    if (selected_row_ >= (int)get_layout().size()) selected_row_ = 0;
    
    int row_len = (int)get_layout()[selected_row_].size();
    if (selected_col_ >= row_len) selected_col_ = row_len - 1;
}

void VirtualKeyboard::navigate_left() {
    if (!active_) return;
    selected_col_--;
    if (selected_col_ < 0) {
        // Move to previous row
        selected_row_--;
        if (selected_row_ < 0) {
            selected_row_ = (int)get_layout().size() - 1;
        }
        
        // Move to end of that row
        selected_col_ = (int)get_layout()[selected_row_].size() - 1;
    }
}

void VirtualKeyboard::navigate_right() {
    if (!active_) return;
    selected_col_++;
    if (selected_col_ >= (int)get_layout()[selected_row_].size()) {
        // Move to next row
        selected_col_ = 0;
        selected_row_++;
        if (selected_row_ >= (int)get_layout().size()) {
            selected_row_ = 0;
        }
    }
}

void VirtualKeyboard::select() {
    if (!active_) return;
    
    const auto& layout = get_layout();
    std::string key = layout[selected_row_][selected_col_];
    
    if (key == "CAPS" || key == "caps") {
        toggle_caps();
    } else if (key == "SYMB" || key == "symb") {
        toggle_symbols();
    } else if (key == "SPACE") {
        space();
    } else if (key == "BACK") {
        backspace();
    } else if (key == "ENTER") {
        if (on_enter_) on_enter_(text_buffer_);
        active_ = false; // Close self? or let caller close? Caller usually handles it.
    } else if (key == "CANCEL") {
        close();
    } else {
        // Normal character
        text_buffer_ += key;
    }
}

void VirtualKeyboard::backspace() {
    if (!text_buffer_.empty()) {
        text_buffer_.pop_back();
    }
}

void VirtualKeyboard::space() {
    text_buffer_ += " ";
}

void VirtualKeyboard::toggle_caps() {
    caps_lock_ = !caps_lock_;
}

void VirtualKeyboard::toggle_symbols() {
    symbols_mode_ = !symbols_mode_;
}

const std::vector<std::vector<std::string>>& VirtualKeyboard::get_layout() const {
    if (symbols_mode_) return LAYOUT_SYMBOLS;
    if (caps_lock_) return LAYOUT_UPPER;
    return LAYOUT_LOWER;
}

} // namespace ui
