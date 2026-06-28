#pragma once
#ifndef HARDWARE_H
#define HARDWARE_H

#include "config.h"
#include "utils.h"
#include <atomic>

class HardwareManager {
public:
    HardwareManager();
    void begin();
    ButtonEventType getButtonEvent();
    unsigned long getExecButtonPressedDuration() const;
    void setLedPattern(LedPatternType pattern, int repeatCount = 0);
    bool isLedPatternActive() const;
    
    // PWM 제어 방식 (0~100)
    void setMosfets(uint8_t pwmValue); 
    
    void shutdownOutputs();
    LedPatternType getCurrentLedPattern() const;

private:
    static void hardwareTask(void* arg);
    void processButtonInput();
    void updateLed();
    void setLed(bool on);

    bool _idButtonState;
    bool _execButtonState;
    unsigned long _lastIdDebounceTime;
    unsigned long _lastExecDebounceTime;
    unsigned long _idButtonPressTimestamp;
    unsigned long _execButtonPressTimestamp;
    unsigned long _bothButtonsPressTimestamp;
    bool _inBothPressSequence;
    std::atomic<ButtonEventType> _currentButtonEvent;
    std::atomic<unsigned long> _execButtonPressedDuration;

    std::atomic<LedPatternType> _currentLedPattern;
    std::atomic<int> _ledTargetBlinkCount;
    std::atomic<unsigned long> _ledPatternStartTime;
    bool _ledState;
    
    std::atomic<uint8_t> _mosfetPwmValue; 
};

#endif // HARDWARE_H