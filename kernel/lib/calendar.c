/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - calendar arithmetic.
 *
 * Pure functions, no kernel state, no locks. Kept separate from the clock so
 * the host test harness can exercise them directly - and because Kaalka keys
 * off broken-down time, which makes a leap-year bug here a security bug there.
 */
#include <rk/time.h>
#include <rk/printf.h>
#include <rk/string.h>

/* ------------------------------------------------------ calendar helpers */

static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

void rk_gmtime(s64 unix_sec, struct rk_tm *out)
{
	s64 days = unix_sec / 86400;
	s64 rem  = unix_sec % 86400;
	if (rem < 0) {
		rem += 86400;
		days--;
	}

	out->hour = (int)(rem / 3600);
	out->min  = (int)((rem % 3600) / 60);
	out->sec  = (int)(rem % 60);
	out->wday = (int)((days + 4) % 7);      /* 1970-01-01 was a Thursday */
	if (out->wday < 0)
		out->wday += 7;

	int year = 1970;
	for (;;) {
		int len = is_leap(year) ? 366 : 365;
		if (days < len)
			break;
		days -= len;
		year++;
	}
	while (days < 0) {
		year--;
		days += is_leap(year) ? 366 : 365;
	}

	out->year = year;
	out->yday = (int)days;

	int m = 0;
	for (;;) {
		int len = mdays[m] + ((m == 1 && is_leap(year)) ? 1 : 0);
		if (days < len)
			break;
		days -= len;
		m++;
	}
	out->month = m + 1;
	out->day   = (int)days + 1;
}

s64 rk_mktime(const struct rk_tm *tm)
{
	s64 days = 0;
	for (int y = 1970; y < tm->year; y++)
		days += is_leap(y) ? 366 : 365;
	for (int y = tm->year; y < 1970; y++)
		days -= is_leap(y) ? 366 : 365;
	for (int m = 0; m < tm->month - 1 && m < 12; m++)
		days += mdays[m] + ((m == 1 && is_leap(tm->year)) ? 1 : 0);
	days += tm->day - 1;
	return days * 86400 + tm->hour * 3600 + tm->min * 60 + tm->sec;
}

size_t rk_format_time(char *buf, size_t n, s64 unix_sec)
{
	struct rk_tm tm;
	rk_gmtime(unix_sec, &tm);
	return (size_t)snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02dZ",
	                        tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec);
}

