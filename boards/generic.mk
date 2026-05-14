# Bare-bones config for boards we don't recognise: chip target only, no LED.
# Override LED_* on the CLI if your board does have a status LED.
TARGET=c6
LED_ENABLE=0
# BOOT button GPIO is unknown for a generic board. Set BOOT_BUTTON_GPIO=N
# on the CLI to enable the long-press factory-reset task, or leave at the
# default (-1, off).
BOOT_BUTTON_GPIO=-1
