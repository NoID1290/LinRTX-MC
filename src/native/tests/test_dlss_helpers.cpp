#include "dlss_sr.hpp"

#include <cassert>
#include <cstdio>

int main() {
    using namespace linrtx;

    // 1. Explicit presets pass through unchanged
    assert(resolveNgxPerfQuality(LINRTX_DLSS_ULTRA_PERFORMANCE, 854, 480, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_UltraPerformance);
    assert(resolveNgxPerfQuality(LINRTX_DLSS_PERFORMANCE, 1280, 720, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_MaxPerf);
    assert(resolveNgxPerfQuality(LINRTX_DLSS_BALANCED, 1486, 836, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_Balanced);
    assert(resolveNgxPerfQuality(LINRTX_DLSS_QUALITY, 1708, 960, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_MaxQuality);
    assert(resolveNgxPerfQuality(LINRTX_DLSS_DLAA, 2560, 1440, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_DLAA);

    // 2. Custom mode selects NGX preset matching the actual scale ratio
    assert(resolveNgxPerfQuality(LINRTX_DLSS_CUSTOM, 854, 480, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_UltraPerformance);
    assert(resolveNgxPerfQuality(LINRTX_DLSS_CUSTOM, 1280, 720, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_MaxPerf);
    assert(resolveNgxPerfQuality(LINRTX_DLSS_CUSTOM, 1486, 836, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_Balanced);
    assert(resolveNgxPerfQuality(LINRTX_DLSS_CUSTOM, 1708, 960, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_MaxQuality);
    assert(resolveNgxPerfQuality(LINRTX_DLSS_CUSTOM, 2560, 1440, 2560, 1440) ==
           NVSDK_NGX_PerfQuality_Value_DLAA);

    // Degenerate dimensions must not divide by zero
    (void)resolveNgxPerfQuality(LINRTX_DLSS_CUSTOM, 0, 0, 0, 0);

    // 3. Even dimension rounding and max-bound clamping
    assert((clampEvenDim(855, 2560) & 1u) == 0u);
    assert(clampEvenDim(855, 2560) == 854u);
    assert(clampEvenDim(854, 2560) == 854u);
    assert(clampEvenDim(1487, 2560) == 1486u);
    assert(clampEvenDim(2560, 2560) == 2560u);
    assert(clampEvenDim(3000, 2560) == 2560u);

    std::printf("PASS: test_dlss_helpers\n");
    return 0;
}
