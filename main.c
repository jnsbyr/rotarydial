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
//                Rotatone commercial product.
//
// Modified     : borishim 2019-04-18
//                https://github.com/borishim/rotarydial
//                Added software debounce, disabled special dial function.
//
// Modified     : Jens B. 2019-12-29
//                https://github.com/jnsbyr/rotarydial
//                Reimplemented and enhanced special dial functions.
//
// This code is distributed under the GNU Public License
// which can be found at http://www.gnu.org/licenses/gpl.txt
//
// DTMF generator logic is loosely based on the AVR314 app note from Atmel
//
//*****************************************************************************

// Uncomment to build with reverse dial
//#define NZ_DIAL

// Modify to disable special functions
#define ENABLE_SPECIAL_FUNCTIONS 1

#include <stdbool.h>
#include <stdint.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <avr/eeprom.h>
#include <string.h>

#include "dtmf.h"

#define PIN_DIAL                    PB2
#define PIN_PULSE                   PB1

#define PINBUF_CHANGED_HIGH(x_)     (((x_) & 0b11000111) == 0b00000111)
#define PINBUF_CHANGED_LOW(x_)      (((x_) & 0b11000111) == 0b11000000)

#define SF_DELAY_MS                 2000L

#define SPEED_DIAL_SIZE             30

#define STATE_DIAL                  0x00
#define STATE_SPECIAL_L1            0x01
#define STATE_SPECIAL_L2            0x02
#define STATE_SPECIAL_L3            0x04
#define STATE_PROGRAM_SD            0x05

#define F_NONE                      0x00
#define F_DETECT_SPECIAL_L1         0x01
#define F_DETECT_SPECIAL_L2         0x02
#define F_DETECT_SPECIAL_L3         0x04
#define F_WDT_AWAKE                 0x08

#define SLEEP_64MS                  0x00
#define SLEEP_128MS                 0x01
#define SLEEP_2S                    0x02
#define SLEEP_4S                    0x03
#define SLEEP_8S                    0x04

#define SPEED_DIAL_COUNT            8 // 8 Positions in total (Redail(3),4,5,6,7,8,9,0)
#define SPEED_DIAL_REDIAL           (SPEED_DIAL_COUNT - 1)

#define L2_STAR                     1
#define L2_POUND                    2
#define L2_REDIAL                   3

typedef struct
{
    uint16_t reg;
    uint8_t bit;
    uint8_t buf;
    bool high;
    bool changed;
} pin_t;

typedef struct
{
    uint8_t state;
    volatile uint8_t flags;
    uint8_t speed_dial_index;
    uint8_t speed_dial_digit_index;
    int8_t speed_dial_digits[SPEED_DIAL_SIZE];
    int8_t redial_digits[SPEED_DIAL_SIZE];
    int8_t dialed_digit;
    pin_t dial_pin;
    pin_t pulse_pin;
} runstate_t;

static void init(void);
static void init_speed_dial(void);
static void process_dialed_digit(runstate_t *rs);
static void dial_speed_dial_number(int8_t index);
static void write_current_speed_dial(int8_t *speed_dial_digits, int8_t index);
static void wdt_timer_start(uint8_t delay);
static void start_sleep(void);
static void wdt_stop(void);
static void update_pin(pin_t *pin, uint16_t reg, uint8_t bit);

// Map speed dial numbers to memory locations
const int8_t _g_speed_dial_loc[] =
{
    0,
    -1 /* 1 - * */,
    -1 /* 2 - # */,
    -1 /* 3 - Redial */,
    1,
    2,
    3,
    4,
    5,
    6
};

int8_t EEMEM _g_speed_dial_eeprom[SPEED_DIAL_COUNT][SPEED_DIAL_SIZE] = { [0 ... (SPEED_DIAL_COUNT - 1)][0 ... SPEED_DIAL_SIZE - 1] = DIGIT_OFF };
runstate_t _g_run_state;

int main(void)
{
    init();

    // monitor input pins
    runstate_t *rs = &_g_run_state;
    volatile uint32_t *delay_counter = &_g_delay_counter;
    while (true)
    {
        // start WDT if dialing has already started
        if (rs->speed_dial_digit_index > 0)
            wdt_timer_start(SLEEP_4S);

        // sleep until interrupted by INT0/PB2 (dial pin low) or WDT
        start_sleep();

        // dialing timeout?
        if (rs->flags == F_WDT_AWAKE)
        {
            // save SD number in EEPROM
            if (rs->speed_dial_digit_index < SPEED_DIAL_SIZE)
            {
                write_current_speed_dial(rs->speed_dial_digits, rs->speed_dial_index);
                if (rs->state == STATE_PROGRAM_SD)
                {
                    // indicate SD was saved with beep
                    dtmf_generate_tone(DIGIT_BEEP, 200);
                }
            }

            // revert to dial state and clear SD number
            rs->state = STATE_DIAL;
            rs->flags = F_NONE;
            init_speed_dial();
            continue;
        }

        // make sure dial pin is low and stable (soft debounce for 500 ms)
        rs->dial_pin.buf = 0b11111111;
        rs->dial_pin.high = true;
        for (int i = 0; i < 5000; i++)
        {
            update_pin(&rs->dial_pin, PINB, PIN_DIAL);
            if (!rs->dial_pin.high)
                break;
            _delay_us(100);
        }
        if (rs->dial_pin.high)
            continue;

        // enable special function detection for regular dialing
        if (ENABLE_SPECIAL_FUNCTIONS && rs->state == STATE_DIAL)
        {
            rs->flags = F_DETECT_SPECIAL_L1;
            *delay_counter = 0;
        }

        // preset pulse pin with current state
        if (bit_is_set(PINB, PIN_PULSE))
        {
          rs->pulse_pin.buf = 0b11111111;
          rs->pulse_pin.high = true;
        }
        else
        {
          rs->pulse_pin.buf = 0b00000000;
          rs->pulse_pin.high = false;
        }

        // take pin samples every 100 µs and count pulses while dial pin is low
        rs->dialed_digit = 0;
        while (!rs->dial_pin.high)
        {
            // special function delay reached?
            switch (rs->flags)
            {
              case F_DETECT_SPECIAL_L1:
                if (*delay_counter >= SF_DELAY_MS * T0_OVERFLOW_PER_MS)
                {
                    // L1 SF mode detected
                    rs->state = STATE_SPECIAL_L1;
                    rs->flags = F_DETECT_SPECIAL_L2;

                    // indicate that we entered L1 SF mode with beep
                    dtmf_generate_tone(DIGIT_BEEP_LOW, 200);
                }
                break;

              case F_DETECT_SPECIAL_L2:
                if (*delay_counter >= SF_DELAY_MS * T0_OVERFLOW_PER_MS)
                {
                    // L2 SF mode detected
                    rs->state = STATE_SPECIAL_L2;
                    rs->flags = F_DETECT_SPECIAL_L3;

                    // indicate that we entered L2 SF mode with asc tone
                    dtmf_generate_tone(DIGIT_TUNE_ASC, 200);
                }
                break;

              case F_DETECT_SPECIAL_L3:
                if (*delay_counter >= SF_DELAY_MS * T0_OVERFLOW_PER_MS)
                {
                    // L3 SF mode detected
                    rs->state = STATE_SPECIAL_L3;
                    rs->flags = F_NONE;

                    // indicate that SF will be canceled
                    dtmf_generate_tone(DIGIT_TUNE_DESC, 800);
                }
                break;

              default:
                rs->flags = F_NONE;
            }

            // check pulse pin
            update_pin(&rs->pulse_pin, PINB, PIN_PULSE);
            if (rs->pulse_pin.high && rs->pulse_pin.changed)
            {
                // pulse detected, cancel SF detection
                rs->flags = F_NONE;
                rs->dialed_digit++;
            }

            // wait and check dial pin again
            _delay_us(100);
            update_pin(&rs->dial_pin, PINB, PIN_DIAL);
        }

        // processed dialed number
        if (rs->dialed_digit > 0 && rs->dialed_digit <= 10)
        {
#ifdef NZ_DIAL
            // NZPO Phones only. 0 is same as GPO but 1-9 are reversed.
            rs->dialed_digit = (10 - rs->dialed_digit);
#else
            if (rs->dialed_digit == 10)
                rs->dialed_digit = 0; // 10 pulses => 0
#endif
            process_dialed_digit(rs);
        }
        else
        {
           // no pulses detected OR count more than 10 pulses
           rs->state = STATE_DIAL;

           // optional: beep to indicate error
           //dtmf_generate_tone(DIGIT_TUNE_DESC, 800);
        }
    }

    return 0;
}

static void process_dialed_digit(runstate_t *rs)
{
    switch (rs->state)
    {
      case STATE_DIAL:
        // Standard (no speed dial, no special function) mode
        if (rs->speed_dial_digit_index < SPEED_DIAL_SIZE)
        {
            // Append next digit to speed dial memory
            rs->speed_dial_digits[rs->speed_dial_digit_index++] = rs->dialed_digit;
        }
        else
        {
            // Clear incomplete speed dial number
            init_speed_dial();
        }
        rs->speed_dial_index = SPEED_DIAL_REDIAL;

        // Generate DTMF code
        dtmf_generate_tone(rs->dialed_digit, DTMF_DURATION_MS);
        break;

      case STATE_SPECIAL_L1:
        rs->state = STATE_DIAL;
        if (rs->dialed_digit == L2_STAR || rs->dialed_digit == L2_POUND)
        {
            // SF 1-* or SF 2-#
            rs->dialed_digit = rs->dialed_digit == L2_STAR? DIGIT_STAR : DIGIT_POUND;
            process_dialed_digit(rs);
        }
        else if (rs->dialed_digit == L2_REDIAL)
        {
            // SF 3 (Redial)
            dial_speed_dial_number(SPEED_DIAL_REDIAL);
        }
        else if (_g_speed_dial_loc[rs->dialed_digit] >= 0)
        {
            // Call speed dial number
            dial_speed_dial_number(_g_speed_dial_loc[rs->dialed_digit]);
        }
        break;

      case STATE_SPECIAL_L2:
        if (_g_speed_dial_loc[rs->dialed_digit] >= 0)
        {
            // Init speed dial recoding
            init_speed_dial();
            rs->speed_dial_index = _g_speed_dial_loc[rs->dialed_digit];
            rs->state = STATE_PROGRAM_SD;
        }
        else
        {
            // Not a speed dial position, revert back to ordinary dial
            rs->state = STATE_DIAL;
            // Beep to indicate error
            dtmf_generate_tone(DIGIT_TUNE_DESC, 800);
        }
        break;

      case STATE_SPECIAL_L3:
        // cancel SF
        rs->state = STATE_DIAL;
        break;

      case STATE_PROGRAM_SD:
        // Do we have too many digits entered?
        if (rs->speed_dial_digit_index >= SPEED_DIAL_SIZE)
        {
            // Exit speed dial mode
            rs->state = STATE_DIAL;
            // Clear incomplete speed dial number
            init_speed_dial();
            // Beep to indicate error
            dtmf_generate_tone(DIGIT_TUNE_DESC, 800);
        }
        else
        {
            // Append next digit to speed dial memory
            rs->speed_dial_digits[rs->speed_dial_digit_index++] = rs->dialed_digit;
            // Generic beep - do not gererate DTMF code
            dtmf_generate_tone(DIGIT_BEEP_LOW, 200);
        }
        break;

      default:
        rs->state = STATE_DIAL;
    }
}

// Dial speed dial number
static void dial_speed_dial_number(int8_t index)
{
    if (index >= 0 && index < SPEED_DIAL_COUNT)
    {
        // load the number
        int8_t speed_dial_digits[SPEED_DIAL_SIZE];
        if (index == SPEED_DIAL_REDIAL)
            memcpy(speed_dial_digits, _g_run_state.redial_digits, SPEED_DIAL_SIZE);
        else
            eeprom_read_block(speed_dial_digits, _g_speed_dial_eeprom[index], SPEED_DIAL_SIZE);

        // dial the number, but skip dialing invalid digits
        for (uint8_t i = 0; i < SPEED_DIAL_SIZE; i++)
        {
            if (speed_dial_digits[i] >= 0 && speed_dial_digits[i] <= DIGIT_POUND)
            {
                dtmf_generate_tone(speed_dial_digits[i], DTMF_DURATION_MS);
                // Pause between DTMF tones
                sleep_ms(DTMF_DURATION_MS);
            }
        }
    }
}

// Save speed dial number
static void write_current_speed_dial(int8_t *speed_dial_digits, int8_t index)
{
    if (index == SPEED_DIAL_REDIAL)
    {
        // save redial number in RAM
        memcpy(_g_run_state.redial_digits, speed_dial_digits, SPEED_DIAL_SIZE);
    }
    else if (index >= 0 && index < SPEED_DIAL_COUNT)
    {
        // save speed dial number on change in EEPROM
        int8_t old_speed_dial_digits[SPEED_DIAL_SIZE];
        eeprom_read_block(old_speed_dial_digits, _g_speed_dial_eeprom[index], SPEED_DIAL_SIZE);
        if (memcmp(old_speed_dial_digits, speed_dial_digits, SPEED_DIAL_SIZE) != 0)
        {
            eeprom_update_block(speed_dial_digits, _g_speed_dial_eeprom[index], SPEED_DIAL_SIZE);
        }
    }
}

static void init(void)
{
    // Program clock prescaller to divide + frequency by 1
    // Write CLKPCE 1 and other bits 0
    CLKPR = _BV(CLKPCE);

    // Write prescaler value with CLKPCE = 0
    CLKPR = 0x00;

    // Enable pull-ups
    PORTB |= (_BV(PIN_DIAL) | _BV(PIN_PULSE));

    // Disable unused modules to save power
    PRR = _BV(PRTIM1) | _BV(PRUSI) | _BV(PRADC);
    ACSR = _BV(ACD);

    // Init DTMF generator
    dtmf_init();

    // Init run state
    _g_run_state.state = STATE_DIAL;

    // Clear redial buffer
    for (uint8_t i = 0; i < SPEED_DIAL_SIZE; i++)
        _g_run_state.redial_digits[i] = DIGIT_OFF;

    // Init speed dialing
    init_speed_dial();

    // Enable interrupts
    sei();
}

static void init_speed_dial(void)
{
    _g_run_state.speed_dial_index = 0;
    _g_run_state.speed_dial_digit_index = 0;
    for (uint8_t i = 0; i < SPEED_DIAL_SIZE; i++)
        _g_run_state.speed_dial_digits[i] = DIGIT_OFF;
}

static void wdt_timer_start(uint8_t delay)
{
    wdt_reset();
    cli();
    MCUSR &= ~(1 << WDRF);           // enable watchdog system reset enable change
    WDTCR |= _BV(WDCE) | _BV(WDE);   // enable watchdog prescaler change

    // change watchog prescaler and enable watchdog interrupt and disable watchdog system reset
    switch (delay)
    {
        case SLEEP_64MS:
            WDTCR = _BV(WDIE) | _BV(WDP1);
            break;
        case SLEEP_128MS:
            WDTCR = _BV(WDIE) | _BV(WDP1) | _BV(WDP0);
            break;
        case SLEEP_2S:
            WDTCR = _BV(WDIE) | _BV(WDP0) | _BV(WDP1) | _BV(WDP2);
            break;
        case SLEEP_4S:
            WDTCR = _BV(WDIE) | _BV(WDP3);
            break;
        case SLEEP_8S:
            WDTCR = _BV(WDIE) | _BV(WDP0) | _BV(WDP3);
            break;
    }
    sei();
}

static void wdt_stop(void)
{
    wdt_reset();
    cli();
    MCUSR &= ~(1 << WDRF);           // enable watchdog system reset enable change
    WDTCR |= _BV(WDCE) | _BV(WDE);   // enable watchdog prescaler change
    WDTCR = 0x00;                    // set watchog prescaler to 16 ms and disable watchdog interrupt and watchdog system reset
    sei();
}

static void start_sleep(void)
{
    GIMSK = _BV(INT0);              // enable INT0
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    cli();                          // stop interrupts to ensure the BOD timed sequence executes as required
    sleep_enable();
    sleep_bod_disable();            // disable brown-out detection (good for 20-25µA)
    sei();                          // ensure interrupts enabled so we can wake up again
    sleep_cpu();                    // go to sleep
    sleep_disable();                // wake up here
    GIMSK = 0;                      // disable INT0
    wdt_stop();                     // disable WDT
}

static void update_pin(pin_t *pin, uint16_t reg, uint8_t bit)
{
    // read pin
    pin->buf = (pin->buf << 1) | (bit_is_set(reg, bit) >> bit);

    // state change detection
    if (PINBUF_CHANGED_LOW(pin->buf))
    {
        // last 3 samples are low
        pin->buf = 0b00000000;
        pin->high = false;
        pin->changed = true;
    }
    else if (PINBUF_CHANGED_HIGH(pin->buf))
    {
        // last 3 samples are high
        pin->buf = 0b11111111;
        pin->high = true;
        pin->changed = true;
    }
    else
        pin->changed = false;
}

// Handler for external interrupt on INT0 (PB2, pin 7, dial)
ISR(INT0_vect)
{
}

// Handler for watchdog interrupt
ISR(WDT_vect)
{
    // mark watchdog wakeup occured in run state
    _g_run_state.flags = F_WDT_AWAKE;
}
