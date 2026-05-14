# ESP32-C3 SuperMini — onboard plain blue LED on GPIO8 (active low typical).
# Uses LEDC PWM so we still get brightness + breathe out of a single-colour LED.
TARGET=c3
LED_ENABLE=1
LED_KIND=2
LED_GPIO=8
LED_BRIGHTNESS=10
# BOOT button — GPIO9 on the SuperMini reference design (active-low).
BOOT_BUTTON_GPIO=9
