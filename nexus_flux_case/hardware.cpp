/**
 * @file hardware.cpp
 * @brief HardwareManager 클래스의 구현입니다.
 * @version 5.0.0
 * @date 2024-08-30
 */
#include "hardware.h"

HardwareManager::HardwareManager() :
    _idButtonState(false), _execButtonState(false),
    _lastIdDebounceTime(0), _lastExecDebounceTime(0),
    _idButtonPressTimestamp(0), _execButtonPressTimestamp(0), _bothButtonsPressTimestamp(0),
    _inBothPressSequence(false),
    _currentButtonEvent(ButtonEventType::NO_EVENT),
    _execButtonPressedDuration(0),
    _currentLedPattern(LedPatternType::LED_OFF),
    _ledTargetBlinkCount(0), _ledPatternStartTime(0),
    _ledState(false), _mosfetState(false)
{}

void HardwareManager::begin() {
    pinMode(ID_BUTTON_PIN, INPUT_PULLUP);
    pinMode(EXEC_BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    pinMode(MOSFET_PIN_1, OUTPUT);
    pinMode(MOSFET_PIN_2, OUTPUT);
    pinMode(BOOST_EN_PIN, OUTPUT); // [NEW] 24V 승압 모듈 EN 핀 활성화

    setLed(false);
    setMosfets(false);

    xTaskCreatePinnedToCore(hardwareTask, "HardwareTask", 4096, this, 2, NULL, 0);
    Log::Info(PSTR("HW: Hardware monitoring task started on Core 0."));
}

void HardwareManager::hardwareTask(void* arg) {
    auto* self = static_cast<HardwareManager*>(arg);
    const TickType_t taskFrequency = pdMS_TO_TICKS(10);
    TickType_t lastWakeTime = xTaskGetTickCount();
    for (;;) {
        self->processButtonInput();
        self->updateLed();
        vTaskDelayUntil(&lastWakeTime, taskFrequency);
    }
}

ButtonEventType HardwareManager::getButtonEvent() {
    ButtonEventType event = _currentButtonEvent.load();
    if (event != ButtonEventType::NO_EVENT) {
        _currentButtonEvent = ButtonEventType::NO_EVENT;
    }
    return event;
}

unsigned long HardwareManager::getExecButtonPressedDuration() const {
    return _execButtonPressedDuration;
}

void HardwareManager::processButtonInput() {
    unsigned long currentTime = millis();

    bool idPressedNow = (digitalRead(ID_BUTTON_PIN) == LOW);
    if (idPressedNow != _idButtonState && (currentTime - _lastIdDebounceTime > DEBOUNCE_DELAY_MS)) {
        _idButtonState = idPressedNow;
        _lastIdDebounceTime = currentTime;
    }

    bool execPressedNow = (digitalRead(EXEC_BUTTON_PIN) == LOW);
    if (execPressedNow != _execButtonState && (currentTime - _lastExecDebounceTime > DEBOUNCE_DELAY_MS)) {
        _execButtonState = execPressedNow;
        _lastExecDebounceTime = currentTime;
    }

    bool bothPressed = _idButtonState && _execButtonState;

    if (_inBothPressSequence) {
        if (!_idButtonState && !_execButtonState) {
            _inBothPressSequence = false;
        }
        return;
    }

    if (bothPressed) {
        if (_bothButtonsPressTimestamp == 0) _bothButtonsPressTimestamp = currentTime;
        _idButtonPressTimestamp = 0;
        _execButtonPressTimestamp = 0;
    }

    if (_bothButtonsPressTimestamp > 0) {
        if (!bothPressed) {
            _bothButtonsPressTimestamp = 0;
        } else if (currentTime - _bothButtonsPressTimestamp >= LONG_PRESS_THRESHOLD_MS) {
            _currentButtonEvent = ButtonEventType::BOTH_BUTTONS_LONG_PRESS;
            _bothButtonsPressTimestamp = 0;
            _inBothPressSequence = true;
            return;
        }
    }

    if (_idButtonState && !_execButtonState && _idButtonPressTimestamp == 0) {
        _idButtonPressTimestamp = currentTime;
    } else if (!_idButtonState && _idButtonPressTimestamp > 0) {
        if (currentTime - _idButtonPressTimestamp >= LONG_PRESS_THRESHOLD_MS) {
            _currentButtonEvent = ButtonEventType::ID_BUTTON_LONG_PRESS_END;
        } else {
            _currentButtonEvent = ButtonEventType::ID_BUTTON_SHORT_PRESS;
        }
        _idButtonPressTimestamp = 0;
    }

    if (_execButtonState && !_idButtonState && _execButtonPressTimestamp == 0) {
        _execButtonPressTimestamp = currentTime;
        _currentButtonEvent = ButtonEventType::EXEC_BUTTON_PRESS;
    } else if (!_execButtonState && _execButtonPressTimestamp > 0) {
        _execButtonPressedDuration = currentTime - _execButtonPressTimestamp;
        _currentButtonEvent = ButtonEventType::EXEC_BUTTON_RELEASE;
        _execButtonPressTimestamp = 0;
    }
}

void HardwareManager::setLedPattern(LedPatternType pattern, int repeatCount) {
    if (_currentLedPattern.load() == pattern && _ledTargetBlinkCount == repeatCount && pattern != LedPatternType::LED_ID_SET_INCREMENT) {
        return;
    }
    _currentLedPattern = pattern;
    _ledTargetBlinkCount = repeatCount;
    _ledPatternStartTime = millis();
    Log::Debug(PSTR("HW: Setting LED pattern to %d, repeat: %d"), static_cast<int>(pattern), repeatCount);
}

void HardwareManager::updateLed() {
    LedPatternType pattern = _currentLedPattern.load();
    if (pattern == LedPatternType::LED_OFF) { if (_ledState) setLed(false); return; }
    if (pattern == LedPatternType::LED_ON) { if (!_ledState) setLed(true); return; }

    unsigned long elapsedTime = millis() - _ledPatternStartTime;

    switch (pattern) {
        case LedPatternType::LED_BOOT_SUCCESS:
            setLed(elapsedTime < LED_BOOT_SUCCESS_ON_MS);
            if (elapsedTime >= LED_BOOT_SUCCESS_ON_MS) _currentLedPattern = LedPatternType::LED_OFF;
            break;
        case LedPatternType::LED_ID_SET_ENTER:
            setLed(elapsedTime < LED_ID_SET_ENTER_ON_MS);
            break;
        case LedPatternType::LED_ID_SET_CONFIRM:
            setLed(elapsedTime < LED_ID_SET_CONFIRM_ON_MS);
             if (elapsedTime >= LED_ID_SET_CONFIRM_ON_MS) {
                _currentLedPattern = LedPatternType::LED_OFF;
            }
            break;
        case LedPatternType::LED_ID_SET_INCREMENT:
            setLed(elapsedTime < LED_ID_SET_INCREMENT_BLINK_MS);
            if (elapsedTime >= LED_ID_SET_INCREMENT_BLINK_MS) _currentLedPattern = LedPatternType::LED_OFF;
            break;
        case LedPatternType::LED_ID_DISPLAY: {
            unsigned long blinkDuration = 2 * LED_ID_BLINK_INTERVAL_MS;
            unsigned long totalDuration = (unsigned long)_ledTargetBlinkCount * blinkDuration;
            if (elapsedTime >= totalDuration) {
                setLed(false);
                _currentLedPattern = LedPatternType::LED_OFF;
            } else {
                setLed((elapsedTime / LED_ID_BLINK_INTERVAL_MS) % 2 == 0);
            }
            break;
        }
        case LedPatternType::LED_WIFI_MODE_TOGGLE: {
            unsigned long totalDuration = (unsigned long)LED_WIFI_MODE_BLINK_COUNT * 2 * LED_WIFI_MODE_BLINK_INTERVAL_MS;
             if (elapsedTime >= totalDuration) {
                _currentLedPattern = LedPatternType::LED_OFF;
            } else {
                setLed((elapsedTime / LED_WIFI_MODE_BLINK_INTERVAL_MS) % 2 == 0);
            }
            break;
        }
        case LedPatternType::LED_ERROR:
            setLed((elapsedTime / LED_ID_BLINK_INTERVAL_MS) % 2 == 0);
            break;
            
        // [NEW] 페어링 대기 중 아주 빠른 점멸 (50ms)
        case LedPatternType::LED_PAIRING:
            setLed((elapsedTime / 50) % 2 == 0);
            break;
            
        default: break;
    }
}

bool HardwareManager::isLedPatternActive() const { return _currentLedPattern.load() != LedPatternType::LED_OFF; }

void HardwareManager::setLed(bool on) {
    if (_ledState != on) {
        _ledState = on;
        digitalWrite(LED_PIN, _ledState);
    }
}

void HardwareManager::setMosfets(bool on) {
    if (_mosfetState.load() != on) {
        _mosfetState = on;
        
        // [NEW] 전자석을 켜기 전 부스트 컨버터를 먼저 활성화
        if (on) {
            digitalWrite(BOOST_EN_PIN, HIGH); // 24V 승압 시작
            delay(10); // 출력단 커패시터에 24V가 충전/안정화될 수 있도록 10ms 대기 (안전장치)
            digitalWrite(MOSFET_PIN_1, HIGH); // 전자석 ON
            digitalWrite(MOSFET_PIN_2, HIGH);
        } else {
            digitalWrite(MOSFET_PIN_1, LOW);  // 전자석 OFF
            digitalWrite(MOSFET_PIN_2, LOW);
            digitalWrite(BOOST_EN_PIN, LOW);  // 부하가 꺼지면 승압 IC도 꺼서 배터리 소모 최소화
        }
        
        Log::Info(PSTR("HW: MOSFETs & BOOST turned %s."), on ? "ON" : "OFF");
    }
}

void HardwareManager::shutdownOutputs() {
    setMosfets(false);
    setLed(false);
    Log::Info(PSTR("HW: All outputs shutdown."));
}

LedPatternType HardwareManager::getCurrentLedPattern() const {
    return _currentLedPattern.load();
}