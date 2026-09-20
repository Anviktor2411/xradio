#include "weather.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"

namespace xr {
namespace weather {

namespace {

void logMsg(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[600];
    snprintf(line, sizeof(line), "XRadio/weather: %s\n", buf);
    XPLMDebugString(line);
}

struct Refs {
    // time
    XPLMDataRef zulu = nullptr, dateDays = nullptr;
    // scalars
    XPLMDataRef visibility = nullptr, pressure = nullptr, temperature = nullptr;
    XPLMDataRef rain = nullptr, snow = nullptr, thermal = nullptr, changeMode = nullptr;
    XPLMDataRef updateNow = nullptr;
    // three cloud layers
    XPLMDataRef cloudType = nullptr, cloudCover = nullptr;
    XPLMDataRef cloudBase = nullptr, cloudTops = nullptr;
    // thirteen air layers
    XPLMDataRef windAlt = nullptr, windSpeed = nullptr, windDir = nullptr;
    XPLMDataRef turbulence = nullptr, tempAlt = nullptr, tempAloft = nullptr;
    XPLMDataRef dewpoint = nullptr;
} g;

bool g_ready = false;
bool g_haveApplied = false;
std::string g_last;

XPLMDataRef find(const char* name, int* missing) {
    XPLMDataRef r = XPLMFindDataRef(name);
    if (!r) {
        ++*missing;
        logMsg("dataref not found: %s", name);
    }
    return r;
}

float getf(XPLMDataRef r) { return r ? XPLMGetDataf(r) : 0.f; }
int   geti(XPLMDataRef r) { return r ? XPLMGetDatai(r) : 0; }

void setf(XPLMDataRef r, float v) { if (r && std::isfinite(v)) XPLMSetDataf(r, v); }
void seti(XPLMDataRef r, int v)   { if (r) XPLMSetDatai(r, v); }

void getArray(XPLMDataRef r, float* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = 0.f;
    if (r) XPLMGetDatavf(r, out, 0, n);
}

// A layer full of NaN would poison the sim's own weather model, and it is
// exactly what a hostile or broken sender would send, so it is dropped here
// rather than written.
void setArray(XPLMDataRef r, const float* in, int n) {
    if (!r) return;
    for (int i = 0; i < n; ++i) if (!std::isfinite(in[i])) return;
    XPLMSetDatavf(r, const_cast<float*>(in), 0, n);
}

}  // namespace

void init() {
    int missing = 0;
    g.zulu       = find("sim/time/zulu_time_sec", &missing);
    g.dateDays   = find("sim/time/local_date_days", &missing);

    g.visibility  = find("sim/weather/region/visibility_reported_sm", &missing);
    g.pressure    = find("sim/weather/region/sealevel_pressure_pas", &missing);
    g.temperature = find("sim/weather/region/sealevel_temperature_c", &missing);
    g.rain        = find("sim/weather/region/rain_percent", &missing);
    g.snow        = find("sim/weather/region/snow_cover", &missing);
    g.thermal     = find("sim/weather/region/thermal_rate_ms", &missing);
    g.changeMode  = find("sim/weather/region/change_mode", &missing);
    // Without this, everything except clouds waits for the sim's own update
    // interval -- a minute of flying in the old sky after the new one arrived.
    g.updateNow   = find("sim/weather/region/update_immediately", &missing);

    g.cloudType  = find("sim/weather/region/cloud_type", &missing);
    g.cloudCover = find("sim/weather/region/cloud_coverage_percent", &missing);
    g.cloudBase  = find("sim/weather/region/cloud_base_msl_m", &missing);
    g.cloudTops  = find("sim/weather/region/cloud_tops_msl_m", &missing);

    g.windAlt    = find("sim/weather/region/wind_altitude_msl_m", &missing);
    g.windSpeed  = find("sim/weather/region/wind_speed_msc", &missing);
    g.windDir    = find("sim/weather/region/wind_direction_degt", &missing);
    g.turbulence = find("sim/weather/region/turbulence", &missing);
    g.tempAlt    = find("sim/weather/region/temperature_altitude_msl_m", &missing);
    g.tempAloft  = find("sim/weather/region/temperatures_aloft_deg_c", &missing);
    g.dewpoint   = find("sim/weather/region/dewpoint_deg_c", &missing);

    // The region datarefs are X-Plane 12's. On anything older they are all
    // missing, and sharing weather is simply not on offer.
    g_ready = (g.zulu != nullptr && g.pressure != nullptr && g.windSpeed != nullptr);
    if (!g_ready) logMsg("this sim has no region weather datarefs, sky sharing is off");
    else if (missing) logMsg("%d weather dataref(s) missing, those parts stay local", missing);
}

bool available() { return g_ready; }

bool read(WeatherPayload& out) {
    if (!g_ready) return false;
    memset(&out, 0, sizeof(out));

    out.zuluTimeSec = getf(g.zulu);
    out.dateDays    = geti(g.dateDays);

    out.visibilitySm       = getf(g.visibility);
    out.seaLevelPressurePa = getf(g.pressure);
    out.seaLevelTempC      = getf(g.temperature);
    out.rainPercent        = getf(g.rain);
    out.snowCover          = getf(g.snow);
    out.thermalRateMs      = getf(g.thermal);
    out.changeMode         = geti(g.changeMode);

    getArray(g.cloudType,  out.cloudType,     kCloudLayers);
    getArray(g.cloudCover, out.cloudCoverage, kCloudLayers);
    getArray(g.cloudBase,  out.cloudBaseM,    kCloudLayers);
    getArray(g.cloudTops,  out.cloudTopsM,    kCloudLayers);

    getArray(g.windAlt,    out.windAltM,     kAirLayers);
    getArray(g.windSpeed,  out.windSpeedMs,  kAirLayers);
    getArray(g.windDir,    out.windDirDeg,   kAirLayers);
    getArray(g.turbulence, out.turbulence,   kAirLayers);
    getArray(g.tempAlt,    out.tempAltM,     kAirLayers);
    getArray(g.tempAloft,  out.tempAloftC,   kAirLayers);
    getArray(g.dewpoint,   out.dewpointC,    kAirLayers);
    return true;
}

void apply(const WeatherPayload& w, float snapSeconds) {
    if (!g_ready) return;

    // Time: a day is 86400 s and the clock wraps, so compare the short way
    // round. Nudging it every packet would fight the sim's own clock and
    // show as a stuttering sun, so it only moves on a real difference.
    if (std::isfinite(w.zuluTimeSec) && w.zuluTimeSec >= 0.f && w.zuluTimeSec < 86400.f) {
        const float mine = getf(g.zulu);
        float diff = w.zuluTimeSec - mine;
        while (diff >  43200.f) diff -= 86400.f;
        while (diff < -43200.f) diff += 86400.f;
        if (std::fabs(diff) > snapSeconds) setf(g.zulu, w.zuluTimeSec);
    }
    if (w.dateDays >= 0 && w.dateDays < 366 && geti(g.dateDays) != w.dateDays)
        seti(g.dateDays, w.dateDays);

    setf(g.visibility,  w.visibilitySm);
    setf(g.pressure,    w.seaLevelPressurePa);
    setf(g.temperature, w.seaLevelTempC);
    setf(g.rain,        w.rainPercent);
    setf(g.snow,        w.snowCover);
    setf(g.thermal,     w.thermalRateMs);
    if (w.changeMode >= 0 && w.changeMode <= 7) seti(g.changeMode, w.changeMode);

    setArray(g.cloudType,  w.cloudType,     kCloudLayers);
    setArray(g.cloudCover, w.cloudCoverage, kCloudLayers);
    setArray(g.cloudBase,  w.cloudBaseM,    kCloudLayers);
    setArray(g.cloudTops,  w.cloudTopsM,    kCloudLayers);

    setArray(g.windAlt,    w.windAltM,     kAirLayers);
    setArray(g.windSpeed,  w.windSpeedMs,  kAirLayers);
    setArray(g.windDir,    w.windDirDeg,   kAirLayers);
    setArray(g.turbulence, w.turbulence,   kAirLayers);
    setArray(g.tempAlt,    w.tempAltM,     kAirLayers);
    setArray(g.tempAloft,  w.tempAloftC,   kAirLayers);
    setArray(g.dewpoint,   w.dewpointC,    kAirLayers);

    seti(g.updateNow, 1);      // now, not at the sim's next update

    char line[96];
    const int hh = (int)(w.zuluTimeSec / 3600.f) % 24;
    const int mm = ((int)(w.zuluTimeSec / 60.f)) % 60;
    snprintf(line, sizeof(line), "sky from the host: %02d:%02dZ, %.0f hPa, %.0f sm vis",
             hh, mm, w.seaLevelPressurePa / 100.0, w.visibilitySm);
    g_last = line;
    if (!g_haveApplied) {
        g_haveApplied = true;
        logMsg("following the host's weather and time");
    }
}

std::string lastApplied() { return g_last; }

}  // namespace weather
}  // namespace xr
