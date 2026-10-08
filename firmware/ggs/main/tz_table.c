#include "tz_table.h"

// ============================================================
// Time zones
//
// Taken from the Spider Farmer app's own sf_timezone.json, so the
// label and the POSIX rules a controller receives are exactly what
// the vendor app would send it.
//
// Not the full list of 461: most entries repeat the same rules, and
// the whole table would be a poor use of flash on a device with 21%
// free. This keeps the zones growers are realistically in, plus one
// representative of every distinct rule string, so any offset is
// still reachable.
//
// The POSIX string is what decides daylight saving. A rule form such
// as "CET-1CEST,M3.5.0,M10.5.0/3" switches on its own; a plain
// "CET-1" pins the clock to standard time all year.
// ============================================================

const tz_entry_t TZ_TABLE[] = {
    { "Africa/Abidjan", "GMT0" },
    { "Africa/Algiers", "CET-1" },
    { "Africa/Blantyre", "CAT-2" },
    { "Africa/Cairo", "EET-2" },
    { "Africa/Casablanca", "<+01>-1" },
    { "Africa/Johannesburg", "SAST-2" },
    { "Africa/Lagos", "WAT-1" },
    { "Africa/Nairobi", "EAT-3" },
    { "America/Adak", "HST10HDT,M3.2.0,M11.1.0" },
    { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Anguilla", "AST4" },
    { "America/Asuncion", "<-04>4<-03>,M10.1.0/0,M3.4.0/0" },
    { "America/Atikokan", "EST5" },
    { "America/Belize", "CST6" },
    { "America/Boa_Vista", "<-04>4" },
    { "America/Bogota", "<-05>5" },
    { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Chihuahua", "MST7MDT,M4.1.0,M10.5.0" },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Godthab", "<-03>3<-02>,M3.5.0/-2,M10.5.0/-1" },
    { "America/Halifax", "AST4ADT,M3.2.0,M11.1.0" },
    { "America/Havana", "CST5CDT,M3.2.0/0,M11.1.0/1" },
    { "America/Lima", "<-05>5" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Mexico_City", "CST6CDT,M4.1.0,M10.5.0" },
    { "America/Miquelon", "<-03>3<-02>,M3.2.0,M11.1.0" },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Noronha", "<-02>2" },
    { "America/Phoenix", "MST7" },
    { "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
    { "America/Sao_Paulo", "<-03>3" },
    { "America/Scoresbysund", "<-01>1<+00>,M3.5.0/0,M10.5.0/1" },
    { "America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0" },
    { "America/Toronto", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Vancouver", "PST8PDT,M3.2.0,M11.1.0" },
    { "Antarctica/Casey", "<+11>-11" },
    { "Antarctica/DumontDUrville", "<+10>-10" },
    { "Antarctica/Mawson", "<+05>-5" },
    { "Antarctica/Troll", "<+00>0<+02>-2,M3.5.0/1,M10.5.0/3" },
    { "Antarctica/Vostok", "<+06>-6" },
    { "Asia/Amman", "EET-2EEST,M2.5.4/24,M10.5.5/1" },
    { "Asia/Anadyr", "<+12>-12" },
    { "Asia/Bangkok", "<+07>-7" },
    { "Asia/Beirut", "EET-2EEST,M3.5.0/0,M10.5.0/0" },
    { "Asia/Chita", "<+09>-9" },
    { "Asia/Colombo", "<+0530>-5:30" },
    { "Asia/Damascus", "EET-2EEST,M3.5.5/0,M10.5.5/0" },
    { "Asia/Dubai", "<+04>-4" },
    { "Asia/Gaza", "EET-2EEST,M3.4.4/48,M10.5.5/1" },
    { "Asia/Hong_Kong", "HKT-8" },
    { "Asia/Jakarta", "WIB-7" },
    { "Asia/Jayapura", "WIT-9" },
    { "Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Asia/Kabul", "<+0430>-4:30" },
    { "Asia/Karachi", "PKT-5" },
    { "Asia/Kathmandu", "<+0545>-5:45" },
    { "Asia/Kolkata", "IST-5:30" },
    { "Asia/Makassar", "WITA-8" },
    { "Asia/Manila", "PST-8" },
    { "Asia/Seoul", "KST-9" },
    { "Asia/Shanghai", "CST-8" },
    { "Asia/Singapore", "<+08>-8" },
    { "Asia/Taipei", "CST-8" },
    { "Asia/Tehran", "<+0330>-3:30<+0430>,J79/24,J263/24" },
    { "Asia/Tokyo", "JST-9" },
    { "Asia/Yangon", "<+0630>-6:30" },
    { "Atlantic/Cape_Verde", "<-01>1" },
    { "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane", "AEST-10" },
    { "Australia/Darwin", "ACST-9:30" },
    { "Australia/Eucla", "<+0845>-8:45" },
    { "Australia/Lord_Howe", "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0" },
    { "Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Perth", "AWST-8" },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Etc/GMT+10", "<-10>10" },
    { "Etc/GMT+11", "<-11>11" },
    { "Etc/GMT+12", "<-12>12" },
    { "Etc/GMT+6", "<-06>6" },
    { "Etc/GMT+7", "<-07>7" },
    { "Etc/GMT+8", "<-08>8" },
    { "Etc/GMT+9", "<-09>9" },
    { "Etc/GMT-13", "<+13>-13" },
    { "Etc/GMT-14", "<+14>-14" },
    { "Etc/GMT-2", "<+02>-2" },
    { "Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Chisinau", "EET-2EEST,M3.5.0,M10.5.0/3" },
    { "Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Dublin", "IST-1GMT0,M10.5.0,M3.5.0/1" },
    { "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Istanbul", "<+03>-3" },
    { "Europe/Kiev", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Moscow", "MSK-3" },
    { "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Pacific/Chatham", "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45" },
    { "Pacific/Easter", "<-06>6<-05>,M9.1.6/22,M4.1.6/22" },
    { "Pacific/Fiji", "<+12>-12<+13>,M11.2.0,M1.2.3/99" },
    { "Pacific/Guam", "ChST-10" },
    { "Pacific/Honolulu", "HST10" },
    { "Pacific/Marquesas", "<-0930>9:30" },
    { "Pacific/Midway", "SST11" },
    { "Pacific/Norfolk", "<+11>-11<+12>,M10.1.0,M4.1.0/3" },
    { "UTC", "UTC0" },
};

const int TZ_TABLE_COUNT = sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]);

const char *tz_posix_for(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < TZ_TABLE_COUNT; i++) {
        if (strcmp(TZ_TABLE[i].name, name) == 0) {
            return TZ_TABLE[i].posix;
        }
    }
    return NULL;
}
