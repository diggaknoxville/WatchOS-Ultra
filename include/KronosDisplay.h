#pragma once
#include <lvgl.h>

// LVGL display + touch driver for the T-Watch Ultra CO5300 AMOLED.
//
//  * two render buffers (horizontal strips) in internal, DMA-capable SRAM
//  * asynchronous QSPI DMA flush: LVGL renders strip N+1 while strip N is
//    being transferred; the wait happens in the flush-wait callback
//  * touch sampled every refresh period (IRQ-gated, so idle cost is zero)
namespace KronosDisplay {
    struct Touch {
        bool pressed;
        int16_t x, y;
        uint32_t ms;               // millis() of the sample
    };

    bool begin();                  // replaces beginLvglHelper(); call on the UI task
    lv_display_t* display();
    lv_indev_t* touch();

    // Single hardware reader shared by LVGL's indev and the gesture engine:
    // pollTouch() samples the controller now, lastTouch() returns the latest
    // sample taken by whoever polled last (LVGL reads every refresh period).
    Touch pollTouch();
    Touch lastTouch();

    // test hook: synthesise a straight finger drag (x0,y0)->(x1,y1) over `ms`,
    // followed by a release. Replaces hardware samples while active.
    void injectSwipe(int x0, int y0, int x1, int y1, uint32_t ms);
}
