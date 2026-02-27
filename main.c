#include <stdint.h>
#include <string.h>

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

#define PMU_ADDR            0x74

#define REG_REF             0x00
#define REG_MODE            0x06

#define REF_5V              0x01a9
#define REF_9V              0x033c
#define REF_15V             0x0599
#define REF_20V             0x0793

#define MODE_OE             (1 << 7)
#define STATUS_SCP          (1 << 7)
#define STATUS_OCP          (1 << 6)
#define STATUS_OVP          (1 << 5)

#define TWI_IDLE            0
#define TWI_ERROR           1
#define TWI_START_SENT      2
#define TWI_REG_SENT        3
#define TWI_DATA_SENT       4
#define TWI_DATA_RECVED     5

#define TWI_READ_REG        0
#define TWI_WRITE_REG       1

#define TWI_ERR_UNKNOWN     0
#define TWI_ERR_ARBLOST     1
#define TWI_ERR_BUSERR      2
#define TWI_ERR_NO_ACK      3

#define PIN_VSEL2           (1 << PIN1)
#define PIN_VSEL1           (1 << PIN2)
#define PIN_ENABLE          (1 << PIN3)
#define PIN_LEDS            (1 << PIN4)

#define RGB(rr, gg, bb)     ((rgb_t){   \
    .r = (rr),                          \
    .g = (gg),                          \
    .b = (bb),                          \
})

#define RGB_OFF             RGB(0x00, 0x00, 0x00)
#define RGB_RED             RGB(0x0a, 0x00, 0x00)
#define RGB_BLUE            RGB(0x00, 0x00, 0x0a)
#define RGB_GREEN           RGB(0x00, 0x0a, 0x00)
#define RGB_WHITE           RGB(0x0a, 0x0a, 0x0a)
#define RGB_YELLOW          RGB(0x0a, 0x0a, 0x00)

typedef uint8_t             byte;
typedef uint16_t            ushort;

typedef struct {
    byte r;
    byte g;
    byte b;
} rgb_t;

typedef struct {
    rgb_t oe;
    rgb_t fault;
    rgb_t volt;
    rgb_t mode;
} led_data_t;

typedef struct {
    ushort ref;
    byte   iout_limit;
    byte   vout_sr;
    byte   vout_fs;
    byte   cdc;
    byte   mode;
    byte   status;
} pmu_regs_t;

static ushort volatile _rtc_tick = 0;
static pmu_regs_t      _pmu_regs = {};

static byte volatile   _twi_op    = TWI_READ_REG;
static byte volatile   _twi_reg   = 0;
static byte volatile   _twi_idx   = 0;
static byte volatile   _twi_len   = 0;
static byte * volatile _twi_buf   = NULL;
static byte volatile   _twi_addr  = 0;
static byte volatile   _twi_error = 0;
static byte volatile   _twi_state = TWI_IDLE;

static void clk_init() {
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, 0);
    while (!(CLKCTRL.MCLKSTATUS & CLKCTRL_OSC20MS_bm));
}

static void rtc_init() {
    RTC.CLKSEL     = RTC_CLKSEL_INT32K_gc;
    RTC.PITCTRLA   = RTC_PERIOD_CYC32_gc | RTC_PITEN_bm;    // 1.024 kHz
    RTC.PITINTCTRL = RTC_PI_bm;
}

static void iop_init() {
    PORTA.DIRSET = PIN_LEDS;
    PORTA.OUTCLR = PIN_LEDS;
    PORTA.DIRCLR = PIN_VSEL1 | PIN_VSEL2 | PIN_ENABLE;
}

static void twi_init() {
    TWI0.MBAUD     = 15;    // 400 kHz
    TWI0.MCTRLA    = TWI_RIEN_bm | TWI_WIEN_bm | TWI_ENABLE_bm;
    TWI0.MSTATUS   = TWI_RIF_bm | TWI_WIF_bm | TWI_BUSSTATE_IDLE_gc;
    PORTB.PIN0CTRL = PORT_PULLUPEN_bm;
    PORTB.PIN1CTRL = PORT_PULLUPEN_bm;
    CPUINT.LVL1VEC = TWI0_TWIM_vect_num;
}

static bool twi_wait() {
    while (_twi_state != TWI_IDLE && _twi_state != TWI_ERROR);
    return _twi_state == TWI_IDLE;
}

static bool twi_start(byte addr, byte reg, byte *buf, byte len, byte op) {
    if (_twi_state != TWI_IDLE) {
        return false;
    }
    _twi_op    = op;
    _twi_reg   = reg;
    _twi_idx   = 0;
    _twi_len   = len;
    _twi_buf   = buf;
    _twi_addr  = addr;
    _twi_state = TWI_START_SENT;
    TWI0.MADDR = addr << 1;
    return true;
}

static bool twi_read_regs(byte addr, byte reg, void *buf, byte len) {
    return twi_start(addr, reg, buf, len, TWI_READ_REG) && twi_wait();
}

static bool twi_write_regs(byte addr, byte reg, const void *buf, byte len) {
    return twi_start(addr, reg, (byte *)buf, len, TWI_WRITE_REG) && twi_wait();
}

#define led_delay_2()       __asm__ volatile ("nop \n nop")
#define led_delay_3()       __asm__ volatile ("nop \n nop \n nop")
#define led_reset_if(b)     do { if (b) PORTA.OUTCLR = PIN_LEDS; } while (0)

#define led_write_bit(v)    do {    \
    PORTA.OUTSET = PIN_LEDS;        \
    led_delay_2();                  \
    led_reset_if(!(v));             \
    led_delay_3();                  \
    led_reset_if(v);                \
    led_delay_3();                  \
} while (0)

static void led_write_colors(const void *data, byte len) {
    for (byte i = 0; i < len; i++) {
        byte v = ((const byte *)data)[i];
        led_write_bit(v & 0x80);
        led_write_bit(v & 0x40);
        led_write_bit(v & 0x20);
        led_write_bit(v & 0x10);
        led_write_bit(v & 0x08);
        led_write_bit(v & 0x04);
        led_write_bit(v & 0x02);
        led_write_bit(v & 0x01);
    }
}

#undef led_write_bit
#undef led_reset_if
#undef led_delay_3
#undef led_delay_2

static void update_pins(byte pins) {
    ushort ref  = _pmu_regs.ref;
    byte   mode = _pmu_regs.mode;
    byte   sel1 = pins & PIN_VSEL1 ? 1 : 0;
    byte   sel2 = pins & PIN_VSEL2 ? 1 : 0;

    /* choose reference voltage */
    switch ((sel2 << 1) | sel1) {
        case 0b00: ref = REF_20V; break;
        case 0b01: ref = REF_15V; break;
        case 0b10: ref = REF_9V;  break;
        case 0b11: ref = REF_5V;  break;
    }

    /* get the output enable status */
    if (pins & PIN_ENABLE) {
        mode |= MODE_OE;
    } else {
        mode &= ~MODE_OE;
    }

    /* update voltage if needed */
    if (ref != _pmu_regs.ref) {
        _pmu_regs.ref = ref;
        twi_write_regs(PMU_ADDR, REG_REF, &ref, 2);
        twi_wait();
    }

    /* update mode if needed */
    if (mode != _pmu_regs.mode) {
        _pmu_regs.mode = mode;
        twi_write_regs(PMU_ADDR, REG_MODE, &mode, 1);
        twi_wait();
    }
}

static void update_leds() {
    byte scp   = (_pmu_regs.status & STATUS_SCP) ? 0x0a : 0;
    byte ocp   = (_pmu_regs.status & STATUS_OCP) ? 0x0a : 0;
    byte ovp   = (_pmu_regs.status & STATUS_OVP) ? 0x0a : 0;
    byte buck  = (_pmu_regs.status & 3)          ? 0x0a : 0;
    byte boost = (_pmu_regs.status & 1) == 0     ? 0x0a : 0;

    /* construct the basic LED data */
    led_data_t data = {
        .oe    = _pmu_regs.mode & MODE_OE ? RGB_RED : RGB_OFF,
        .fault = RGB(scp, ovp, ocp),
        .volt  = RGB_WHITE,
        .mode  = RGB(buck, boost, 0),
    };

    /* select color for voltage */
    switch (_pmu_regs.ref) {
        case REF_5V  : data.volt = RGB_RED    ; break;
        case REF_9V  : data.volt = RGB_YELLOW ; break;
        case REF_15V : data.volt = RGB_BLUE   ; break;
        case REF_20V : data.volt = RGB_GREEN  ; break;
    }

    /* update status */
    cli();
    led_write_colors(&data, sizeof(data));
    PORTA.OUTCLR = PIN_LEDS;
    sei();
}

static void update_pmu() {
    if (_rtc_tick >= 10) {
        _rtc_tick = 0;
        twi_read_regs(PMU_ADDR, 0, &_pmu_regs, sizeof(pmu_regs_t));
        update_leds();
    }
}

int main() {
    _pmu_regs.ref = REF_5V;
    _pmu_regs.mode = 0;

    /* initialize hardware */
    clk_init();
    rtc_init();
    iop_init();
    twi_init();
    sei();

    /* startup as 5V with no output */
    twi_write_regs(PMU_ADDR, REG_REF, &_pmu_regs.ref, 2);
    twi_write_regs(PMU_ADDR, REG_MODE, &_pmu_regs.mode, 1);

    /* main event loop */
    for (;;) {
        update_pmu();
        update_pins(PORTA.IN & (PIN_VSEL1 | PIN_VSEL2 | PIN_ENABLE));
    }
}

ISR(RTC_PIT_vect) {
    _rtc_tick++;
    RTC.PITINTFLAGS = RTC_PI_bm;
}

static void twi_isr_abort(byte error) {
    TWI0.MCTRLB = TWI_FLUSH_bm | TWI_MCMD_STOP_gc;
    _twi_state = TWI_ERROR;
    _twi_error = error;
}

static void twi_isr_write_once() {
    if (_twi_idx < _twi_len) {
        TWI0.MDATA = _twi_buf[_twi_idx++];
        _twi_state = TWI_DATA_SENT;
    } else {
        TWI0.MCTRLB = TWI_MCMD_STOP_gc;
        _twi_state = TWI_IDLE;
    }
}

static void twi_isr_check_read() {
    if (_twi_idx >= _twi_len) {
        TWI0.MCTRLB = TWI_ACKACT_NACK_gc | TWI_MCMD_STOP_gc;
        _twi_state = TWI_IDLE;
    } else {
        TWI0.MCTRLB = TWI_MCMD_RECVTRANS_gc;
    }
}

static void twi_isr_handle_reg_sent() {
    switch (_twi_op) {
        case TWI_READ_REG: {
            _twi_state = TWI_DATA_RECVED;
            TWI0.MADDR = (_twi_addr << 1) | 1;
            break;
        }
        case TWI_WRITE_REG: {
            twi_isr_write_once();
            break;
        }
        default: {
            twi_isr_abort(TWI_ERR_UNKNOWN);
            break;
        }
    }
}

#define twi_isr_check_ack() do {        \
    if (status & TWI_RXACK_bm) {        \
        twi_isr_abort(TWI_ERR_NO_ACK);  \
        return;                         \
    }                                   \
} while (0)

ISR(TWI0_TWIM_vect) {
    byte state = _twi_state;
    byte status = TWI0.MSTATUS;

    /* check for arbitration lost */
    if (status & TWI_ARBLOST_bm) {
        twi_isr_abort(TWI_ERR_ARBLOST);
        return;
    }

    /* check for arbitration lost */
    if (status & TWI_BUSERR_bm) {
        twi_isr_abort(TWI_ERR_BUSERR);
        return;
    }

    /* dispatch on state */
    switch (state) {
        case TWI_START_SENT: {
            twi_isr_check_ack();
            TWI0.MDATA = _twi_reg;
            _twi_state = TWI_REG_SENT;
            break;
        }
        case TWI_REG_SENT: {
            twi_isr_check_ack();
            twi_isr_handle_reg_sent();
            break;
        }
        case TWI_DATA_SENT: {
            twi_isr_check_ack();
            twi_isr_write_once();
            break;
        }
        case TWI_DATA_RECVED: {
            _twi_buf[_twi_idx++] = TWI0.MDATA;
            twi_isr_check_read();
            break;
        }
        default: {
            twi_isr_abort(TWI_ERR_UNKNOWN);
            break;
        }
    }
}
