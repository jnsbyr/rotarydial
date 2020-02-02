//*****************************************************************************
// Title        : Pulse to tone (DTMF) converter
// Author       : Boris Cherkasskiy
//                http://boris0.blogspot.ca/2013/09/rotary-dial-for-digital-age.html
// Created      : 2011-10-24
//
// Modified     : Arnie Weber 2015-06-22
//                https://bitbucket.org/310weber/rotary_dial/
//                NOTE: This code is not compatible with Boris's original hardware
//                due to changed pin-out (see Eagle files for details)
//
// Modified     : Matthew Millman 2018-05-29
//                http://tech.mattmillman.com/
//                Cleaned up implementation, modified to work more like the
//                Rotatone product.
//
// Modified     : borishim 2019-04-18
//                https://github.com/borishim/rotarydial
//                Modified for use with 8 MHz crystal.
//
// Modified     : Jens B. 2020-01-03
//                https://github.com/jnsbyr/rotarydial
//                Support 4 and 8 MHz crystals.
//
// This code is distributed under the GNU Public License
// which can be found at http://www.gnu.org/licenses/gpl.txt
//
// DTMF generator logic is loosely based on the AVR314 app note from Atmel
//
//*****************************************************************************

#ifndef __DTMF_H__
#define __DTMF_H__

#define DIGIT_BEEP          -10
#define DIGIT_BEEP_LOW      -13
#define DIGIT_TUNE_ASC      -11
#define DIGIT_TUNE_DESC     -12
#define DIGIT_OFF           -1
#define DIGIT_STAR          10
#define DIGIT_POUND         11

#define DTMF_DURATION_MS    200

#define NUM_SAMPLES         128     // Number of samples in lookup table

#if (CLOCK_SOURCE == 0 || F_CPU == 8000000L)
// PWM frequency = 8MHz/256 = 31250Hz; overflow cycles per MS = 31
#define T0_OVERFLOW_PER_MS  31
#elif (F_CPU == 4000000L)
// external clock, PWM frequency = 4MHz/256 = 15625Hz; overflow cycles per MS = 15
#define T0_OVERFLOW_PER_MS  15
#endif

#define PIN_PWM_OUT         PB0     // PB0 (OC0A) as PWM output

void dtmf_init(void);
void dtmf_generate_tone(int8_t digit, uint16_t duration_ms);
void sleep_ms(uint16_t msec);

extern volatile uint32_t _g_delay_counter;

#endif /* __DTMF_H__ */
