#ifndef RTC_S3_TEST_UTILS_H
#define RTC_S3_TEST_UTILS_H
#include <stdint.h>
/* Minimal declaration matching the bootloader's polled-UART printf(). */
int printf(const uint8_t *msg);
#endif
