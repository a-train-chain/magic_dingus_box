#pragma once

#include <string>
#include <vector>
#include <functional>
#include <string>

namespace ui {

// Forward decl
class Renderer;

class VirtualKeyboard {
public:
    using OnEnterCallback = std::function<void(const std::string&)>;
    using OnCancelCallback = std::function<void()>;

    VirtualKeyboard();

    void open(const std::string& initial_text, const std::string& title, OnEnterCallback on_enter, OnCancelCallback on_cancel);
    void close();

    bool is_active() const { return active_; }
    const std::string& get_text() const { return text_buffer_; }
    const std::string& get_title() const { return title_; }

    // Navigation
    void navigate_up();
    void navigate_down();
    void navigate_left();
    void navigate_right();
    
    // Action (Select key)
    void select();
    
    // Special actions
    void backspace();
    void space();
    void toggle_caps();
    void toggle_symbols();

    // Append a literal character to the buffer. Bypasses the on-screen
    // layout's focus-driven select() so external sources (Phone Remote)
    // can inject any character directly. No-op when not active.
    void type_char(char c);

    // Wipe text_buffer_ in one shot. Used by Phone Remote's "×" clear
    // affordance and paste handling. No-op when not active.
    void clear_buffer();

    // Submit the current buffer: fire on_enter_(text_buffer_) if a
    // callback is set, then close the keyboard (active_ = false).
    // This mirrors select()'s ENTER-key path so the Phone Remote's
    // "enter" message and the on-screen keyboard's ENTER key produce
    // identical state transitions. No-op when not active.
    void commit();

    // Accessors for renderer
    int get_selected_row() const { return selected_row_; }
    int get_selected_col() const { return selected_col_; }
    const std::vector<std::vector<std::string>>& get_layout() const;

private:
    bool active_;
    std::string text_buffer_;
    std::string title_;
    
    OnEnterCallback on_enter_;
    OnCancelCallback on_cancel_;
    
    int selected_row_;
    int selected_col_;
    
    bool caps_lock_;
    bool symbols_mode_;
    
    // Layouts
    // We'll store them as static or member members
    // Rows of keys. Special keys: "SPACE", "BACK", "ENTER", "CAPS", "SYMB"
    // We'll handle rendering of special keys in the renderer
};

} // namespace ui
