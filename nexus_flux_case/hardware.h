/**
 * @file hardware.h
 * @brief HardwareManager 클래스의 헤더 파일입니다.
 * @version 5.0.0
 * @date 2024-08-30
 */
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
    void setMosfets(bool on); // `setMosfetState`에서 이름 변경
    void shutdownOutputs();
    LedPatternType getCurrentLedPattern() const;

private:
    static void hardwareTask(void* arg);
    void processButtonInput();
    void updateLed();
    void setLed(bool on);

    // Button states
    bool _idButtonState;
    bool _execButtonState;
    unsigned long _lastIdDebounceTime;
    unsigned long _lastExecDebounceTime;
    unsigned long _idButtonPressTimestamp;
    unsigned long _execButtonPressTimestamp;
    unsigned long _bothButtonsPressTimestamp;
    bool _inBothPressSequence;
    std::atomic<ButtonEventType> _currentButtonEvent;
    unsigned long _execButtonPressedDuration;

    // LED states
    std::atomic<LedPatternType> _currentLedPattern;
    int _ledTargetBlinkCount;
    unsigned long _ledPatternStartTime;
    bool _ledState;
    std::atomic<bool> _mosfetState;
};

#endif // HARDWARE_H