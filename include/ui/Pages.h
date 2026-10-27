#pragma once
// Page builders. Each creates its root with ui::pageRoot(screen) and registers
// itself with the navigator (nav::registerPage).
#include <lvgl.h>

namespace pages {
void buildFace(lv_obj_t* scr);
void buildAiLimits(lv_obj_t* scr);
void buildLauncher(lv_obj_t* scr);
void buildActivity(lv_obj_t* scr);
void buildAgenda(lv_obj_t* scr);
void buildPosition(lv_obj_t* scr);
void buildLora(lv_obj_t* scr);
void buildSystem(lv_obj_t* scr);
void buildControl(lv_obj_t* scr);
void buildNotifications(lv_obj_t* scr);
void buildConnect(lv_obj_t* scr);
void buildStopwatch(lv_obj_t* scr);
void buildPower(lv_obj_t* scr);
void buildFaces(lv_obj_t* scr);
void buildSettings(lv_obj_t* scr);
void buildAppMedia(lv_obj_t* scr);
void buildCalib(lv_obj_t* scr);
void buildGames(lv_obj_t* scr);        // games folder + game host (src/ui/games)

// watch face helpers used by the engine and the face picker
void faceNext();
void faceSelect(int idx);              // stores the choice and shows a toast
bool faceShowing();
const char* faceName(int idx);         // "Modular", "Bauhaus", ...
const char* faceDescription(int idx);  // one-line German description
}
