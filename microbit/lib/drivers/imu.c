/*
 * imu.c - LSM303AGR accelerometer over the micro:bit v2 internal I2C.
 *
 * Internal I2C (nRF52833 TWIM0):  SCL = P0.08, SDA = P0.16.
 * Uses TWIM EasyDMA write-then-read for register access.
 */

#include "imu.h"
#include "gpio.h"

/* --- TWIM0 peripheral (base 0x40003000) --- */
#define TWIM_BASE           0x40003000UL
#define T(off)              (TWIM_BASE + (off))

#define TWIM_TASKS_STARTRX  T(0x000)
#define TWIM_TASKS_STARTTX  T(0x008)
#define TWIM_TASKS_STOP     T(0x014)
#define TWIM_EVENTS_STOPPED T(0x104)
#define TWIM_EVENTS_ERROR   T(0x124)
#define TWIM_SHORTS         T(0x200)
#define TWIM_ERRORSRC       T(0x4C4)
#define TWIM_ENABLE         T(0x500)
#define TWIM_PSEL_SCL       T(0x508)
#define TWIM_PSEL_SDA       T(0x50C)
#define TWIM_FREQUENCY      T(0x524)
#define TWIM_RXD_PTR        T(0x534)
#define TWIM_RXD_MAXCNT     T(0x538)
#define TWIM_TXD_PTR        T(0x544)
#define TWIM_TXD_MAXCNT     T(0x548)
#define TWIM_ADDRESS        T(0x588)

#define TWIM_ENABLE_VAL     6
#define TWIM_FREQ_100K      0x01980000

#define SHORT_LASTTX_STARTRX (1UL << 7)
#define SHORT_LASTTX_STOP    (1UL << 9)
#define SHORT_LASTRX_STOP    (1UL << 12)

#define PIN_SCL             PIN(0, 8)
#define PIN_SDA             PIN(0, 16)

/* I2C 7-bit address and registers for the LSM303AGR accelerometer */
#define ACC_ADDR            0x19
#define REG_WHO_AM_I        0x0F
#define REG_CTRL_REG1_A     0x20
#define REG_CTRL_REG4_A     0x23
#define REG_OUT_X_L_A       0x28
#define AUTO_INC            0x80        /* sub-address auto-increment bit */

static UB tx_buf[2] __attribute__((aligned(4)));
static UB rx_buf[8] __attribute__((aligned(4)));

static INT twim_wait(void)
{
    while (in_w(TWIM_EVENTS_STOPPED) == 0 && in_w(TWIM_EVENTS_ERROR) == 0) {}
    if (in_w(TWIM_EVENTS_ERROR) != 0) {
        out_w(TWIM_EVENTS_ERROR, 0);
        out_w(TWIM_TASKS_STOP, 1);
        while (in_w(TWIM_EVENTS_STOPPED) == 0) {}
        out_w(TWIM_EVENTS_STOPPED, 0);
        return -1;
    }
    out_w(TWIM_EVENTS_STOPPED, 0);
    return 0;
}

static INT twim_write_reg(UB reg, UB val)
{
    tx_buf[0] = reg;
    tx_buf[1] = val;

    out_w(TWIM_TXD_PTR, (UW)tx_buf);
    out_w(TWIM_TXD_MAXCNT, 2);
    out_w(TWIM_SHORTS, SHORT_LASTTX_STOP);

    out_w(TWIM_EVENTS_STOPPED, 0);
    out_w(TWIM_EVENTS_ERROR, 0);
    out_w(TWIM_TASKS_STARTTX, 1);
    return twim_wait();
}

static INT twim_read_regs(UB reg, UB *buf, UINT n)
{
    UINT i;

    tx_buf[0] = reg;

    out_w(TWIM_TXD_PTR, (UW)tx_buf);
    out_w(TWIM_TXD_MAXCNT, 1);
    out_w(TWIM_RXD_PTR, (UW)rx_buf);
    out_w(TWIM_RXD_MAXCNT, n);
    /* write the sub-address, repeated-start into a read, stop at the end */
    out_w(TWIM_SHORTS, SHORT_LASTTX_STARTRX | SHORT_LASTRX_STOP);

    out_w(TWIM_EVENTS_STOPPED, 0);
    out_w(TWIM_EVENTS_ERROR, 0);
    out_w(TWIM_TASKS_STARTTX, 1);
    if (twim_wait() != 0) {
        return -1;
    }

    for (i = 0; i < n; i++) {
        buf[i] = rx_buf[i];
    }
    return 0;
}

INT imu_init(void)
{
    UB who = 0;

    /* configure SCL/SDA: input buffer connected, pull-up, S0D1 drive */
    out_w(GPIOH_PINCNF(GPIO0_BASE, PIN_NUM(PIN_SCL)), 0x0000060C);
    out_w(GPIOH_PINCNF(GPIO0_BASE, PIN_NUM(PIN_SDA)), 0x0000060C);

    out_w(TWIM_ENABLE, 0);
    out_w(TWIM_PSEL_SCL, PIN_NUM(PIN_SCL));   /* both on port 0 */
    out_w(TWIM_PSEL_SDA, PIN_NUM(PIN_SDA));
    out_w(TWIM_FREQUENCY, TWIM_FREQ_100K);
    out_w(TWIM_ADDRESS, ACC_ADDR);
    out_w(TWIM_ENABLE, TWIM_ENABLE_VAL);

    if (twim_read_regs(REG_WHO_AM_I, &who, 1) != 0) {
        return -1;
    }

    /* CTRL_REG1_A = 0x57: 100 Hz, normal mode, X/Y/Z enabled */
    if (twim_write_reg(REG_CTRL_REG1_A, 0x57) != 0) {
        return -1;
    }

    return (INT)who;
}

INT imu_read(H *x, H *y, H *z)
{
    UB d[6];

    if (twim_read_regs(REG_OUT_X_L_A | AUTO_INC, d, 6) != 0) {
        return -1;
    }

    /* little-endian, left-justified 16-bit (low byte first) */
    *x = (H)((d[1] << 8) | d[0]);
    *y = (H)((d[3] << 8) | d[2]);
    *z = (H)((d[5] << 8) | d[4]);
    return 0;
}

INT imu_set_fullscale(INT g)
{
    UB v;   /* CTRL_REG4_A: BDU=1 (block data update), FS in bits [5:4] */

    switch (g) {
    case 2:  v = 0x80; break;   /* +/-2g  */
    case 4:  v = 0x90; break;   /* +/-4g  */
    case 8:  v = 0xA0; break;   /* +/-8g  */
    case 16: v = 0xB0; break;   /* +/-16g */
    default: return -1;
    }
    return twim_write_reg(REG_CTRL_REG4_A, v);
}
