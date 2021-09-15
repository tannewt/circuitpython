// Stub for peripherals/touch.h
// include the correct file based on TARGET
// Seon Rozenblum - Unexpected Maker
// Sep 14 2021

#ifdef CONFIG_IDF_TARGET_ESP32S2
#include "peripherals/esp32s2/touch.h"
#endif
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "peripherals/esp32s3/touch.h"
#endif