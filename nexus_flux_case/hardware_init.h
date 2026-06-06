#ifndef HARDWARE_INIT_H
#define HARDWARE_INIT_H

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config_t.h" // For OLED_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN, VIB_MOTOR_PIN, BATTERY_ADC_PIN
#include "utils_t.h"  // For logPrintf
// [REMOVED] Button class, button objects extern declarations moved to hardware_t.h or hardware_buttons.h
// #include "hardware_buttons.h" // For Button class, button objects

// Hardware Initialization Functions
bool initHardware();
bool initDisplay();
void initVibrationMotor();
void initBatteryMonitor();

#endif // HARDWARE_INIT_H
