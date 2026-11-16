// tube_renderer_passes.cpp - TubeRenderer's frame: the passes a tube::PassPlan draws
// (composite, Task 11) and the hardware validation of every program (Task 14). Empty until
// then: nothing calls TubeRenderer yet, so composite() and validate_all_programs() are
// declared in tube_renderer.h and defined here by those tasks. KIOSK-ONLY (GL).
//
// What this file builds on (tube_renderer_internal.h): the program slots and their uniform
// tables, the drawn-program bookkeeping (mark_drawn, settle_marker), the render targets
// (ensure_targets), the mask and glass (upload_mask, bake_glass) and the GL state
// save/restore every path uses (TubeGlState).

#include "tube_renderer.h"
#include "tube_renderer_internal.h"
