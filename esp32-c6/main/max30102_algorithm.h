#ifndef MAX30102_ALGORITHM_H
#define MAX30102_ALGORITHM_H

/*
 * Heart rate / SpO2 calculation algorithm, ported from Maxim Integrated's
 * reference implementation (MAXREFDES117), the same one shipped as
 * spo2_algorithm.cpp in the SparkFun MAX3010x Arduino library and used by the
 * proven Arduino reference firmware for this bracelet. Only the Arduino.h
 * dependency and the AVR 16-bit fallback were removed so it builds under
 * ESP-IDF; the math is untouched. See max30102_algorithm.c for the license.
 */

#include <stdint.h>

/* Effective sample rate the algorithm assumes, i.e. the FIFO output rate after
 * the MAX30102's own on-chip averaging (100 Hz raw / 4-sample average = 25 Hz).
 * Keep this matched to max30102.c's sensor configuration. */
#define MAX30102_ALG_SAMPLE_RATE_HZ 25
#define MAX30102_ALG_BUFFER_SIZE (MAX30102_ALG_SAMPLE_RATE_HZ * 4)

void maxim_heart_rate_and_oxygen_saturation(uint32_t *pun_ir_buffer, int32_t n_ir_buffer_length, uint32_t *pun_red_buffer,
											 int32_t *pn_spo2, int8_t *pch_spo2_valid, int32_t *pn_heart_rate,
											 int8_t *pch_hr_valid);

#endif
