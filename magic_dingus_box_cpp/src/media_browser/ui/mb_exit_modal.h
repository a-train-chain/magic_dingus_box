#pragma once

namespace ui { class Renderer; }

namespace media_browser::ui {

// "Exit Marquee?" yes/no confirm modal. Rendered above all MB screens.
// Owned by main.cpp's MB dispatch loop.
class ExitModal {
public:
    enum class Result {
        Pending,
        Cancel,
        Exit,
    };

    void open();
    void close();
    bool is_open() const { return open_; }

    // Input handlers. Return true if input was consumed.
    // When the modal commits (Cancel or Exit chosen), last_result_ is set
    // and the modal closes. Caller polls last_result() and clears it.
    bool on_rotate(int delta);    // toggle button focus
    bool on_select();              // commit highlighted button
    bool on_btn1();                // toggle button focus (PREV)
    bool on_btn2();                // again-tap → quick-confirm Exit
    bool on_btn3();                // toggle button focus (NEXT)
    bool on_btn4();                // same as Cancel

    Result last_result() const { return last_result_; }
    void clear_result() { last_result_ = Result::Pending; }

    void render(::ui::Renderer& r, int screen_w, int screen_h);

private:
    bool open_ = false;
    bool focus_exit_ = false;        // false = Cancel focused (default), true = Exit focused
    Result last_result_ = Result::Pending;
};

}  // namespace media_browser::ui
