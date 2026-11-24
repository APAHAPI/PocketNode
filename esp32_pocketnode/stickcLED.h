#pragma once

#ifndef stickcLED_H
#define stickcLED_H

#if defined(POCKETNODE_M5STICKC) || defined(POCKETNODE_M5STICKCP2)

#include "configs.h"
#include "settings.h"

#include <Arduino.h>


extern Settings settings_obj;

class stickcLED {

    public:
        void RunSetup();
        void main();
        void attackLED();
        void sniffLED();
        void offLED();
};

#endif
#endif  /* stickcLED_H */
