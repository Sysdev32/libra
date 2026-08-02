//
// Created by adam on 8/3/26.
//
// Hardware I/O Ports for CMOS
#define CMOS_INDEX        0x70
#define CMOS_DATA         0x71
#define CMOS_REG_CENTURY  0x32
#include <stdint.h>
#include <fs/vfs.h>
#include <hals/systemio.h>
static const uint16_t days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};
static uint8_t read_cmos_register(uint8_t reg) {
    outb(CMOS_INDEX, reg | 0x80);
    return inb(CMOS_DATA);
}

static int is_update_in_progress(void) {
    return (read_cmos_register(0x0A) & 0x80);
}

static int32_t calculate_epoch_seconds(uint32_t year, uint32_t month, uint32_t day,
                                       uint32_t hour, uint32_t minute, uint32_t second)
{
    int32_t total_days = 0;
    month -= 1;

    for (uint32_t y = 1970; y < year; y++) {
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
            total_days += 366;
        } else {
            total_days += 365;
        }
    }

    total_days += days_before_month[month];

    if (month > 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        total_days++;
    }

    total_days += (day - 1);
    return (total_days * 86400) + (hour * 3600) + (minute * 60) + second;
}

void rtc_get_time(struct timespec *ts) {
    while (is_update_in_progress());

    uint8_t sec     = read_cmos_register(0x00);
    uint8_t min     = read_cmos_register(0x02);
    uint8_t hr      = read_cmos_register(0x04);
    uint8_t dy      = read_cmos_register(0x07);
    uint8_t mo      = read_cmos_register(0x08);
    uint8_t yr      = read_cmos_register(0x09);
    uint8_t century = read_cmos_register(CMOS_REG_CENTURY);
    uint8_t regB    = read_cmos_register(0x0B);

    if (!(regB & 0x04)) {
        sec     = (sec & 0x0F)     + ((sec / 16) * 10);
        min     = (min & 0x0F)     + ((min / 16) * 10);
        hr      = ((hr & 0x0F) + (((hr & 0x70) / 16) * 10)) | (hr & 0x80);
        dy      = (dy & 0x0F)     + ((dy / 16) * 10);
        mo      = (mo & 0x0F)     + ((mo / 16) * 10);
        yr      = (yr & 0x0F)     + ((yr / 16) * 10);
        century = (century & 0x0F) + ((century / 16) * 10);
    }

    if (!(regB & 0x02) && (hr & 0x80)) {
        hr = ((hr & 0x7F) + 12) % 24;
    }

    uint32_t full_year = (century == 0) ? (2000 + yr) : ((century * 100) + yr);

    ts->tv_sec  = calculate_epoch_seconds(full_year, mo, dy, hr, min, sec);
    ts->tv_nsec = 0;
}