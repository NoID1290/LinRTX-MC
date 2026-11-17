package dev.linrtx.mc;

import dev.linrtx.mc.config.LinRtxEnums.DlssMode;
import dev.linrtx.mc.natives.LinRtxNative;
import dev.linrtx.mc.natives.NativeExtractor;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.lang.foreign.MemoryLayout.PathElement;
import java.nio.file.Files;
import java.nio.file.Path;

import static org.junit.jupiter.api.Assertions.*;

class NativeStructLayoutTest {
    @Test
    void ffmStructLayoutsMatchNativeAbi() {
        assertEquals(72L, LinRtxNative.INIT_CONFIG_LAYOUT.byteSize());
        assertEquals(64L, LinRtxNative.INIT_CONFIG_LAYOUT.byteOffset(PathElement.groupElement("logDir")));

        assertEquals(600L, LinRtxNative.FRAME_CONFIG_LAYOUT.byteSize());
        assertEquals(108L, LinRtxNative.FRAME_CONFIG_LAYOUT.byteOffset(PathElement.groupElement("colorInTexGlId")));
        assertEquals(208L, LinRtxNative.FRAME_CONFIG_LAYOUT.byteOffset(PathElement.groupElement("viewToClip")));
        assertEquals(400L, LinRtxNative.FRAME_CONFIG_LAYOUT.byteOffset(PathElement.groupElement("prevClipToClip")));
        assertEquals(464L, LinRtxNative.FRAME_CONFIG_LAYOUT.byteOffset(PathElement.groupElement("denoiseMode")));
        assertEquals(532L, LinRtxNative.FRAME_CONFIG_LAYOUT.byteOffset(PathElement.groupElement("worldToView")));

        assertEquals(96L, LinRtxNative.DENOISE_CONFIG_LAYOUT.byteSize());
        assertEquals(64L, LinRtxNative.DENOISE_CONFIG_LAYOUT.byteOffset(PathElement.groupElement("temporalBlend")));
        assertEquals(88L, LinRtxNative.DENOISE_CONFIG_LAYOUT.byteOffset(PathElement.groupElement("cameraNear")));

        assertEquals(1104L, LinRtxNative.STATUS_LAYOUT.byteSize());
        assertEquals(64L, LinRtxNative.STATUS_LAYOUT.byteOffset(PathElement.groupElement("gpuName")));
        assertEquals(640L, LinRtxNative.STATUS_LAYOUT.byteOffset(PathElement.groupElement("lastErrorMsg")));
        assertEquals(896L, LinRtxNative.STATUS_LAYOUT.byteOffset(PathElement.groupElement("dlssdSupported")));
        assertEquals(976L, LinRtxNative.STATUS_LAYOUT.byteOffset(PathElement.groupElement("denoiserStatus")));
    }

    @Test
    void loadsLibLinRtxAndQueriesOptimalScale() {
        Path soPath = Path.of("src/main/resources/natives/linux-x86_64/liblinrtx.so").toAbsolutePath();
        assertTrue(LinRtxNative.loadLibrary(soPath), () -> "Failed loading " + soPath + ": " + LinRtxNative.getLoadError());

        int[] q1440 = LinRtxNative.getOptimalScale(DlssMode.QUALITY.nativeId, 2560, 1440);
        assertNotNull(q1440);
        assertEquals(1708, q1440[0]);
        assertEquals(960, q1440[1]);

        int[] dlaa1080 = LinRtxNative.getOptimalScale(DlssMode.DLAA.nativeId, 1920, 1080);
        assertNotNull(dlaa1080);
        assertEquals(1920, dlaa1080[0]);
        assertEquals(1080, dlaa1080[1]);

        // Uninitialized engine should fail gracefully with -1 without crashing
        assertEquals(-1, LinRtxNative.evaluateDenoise(LinRtxNative.scratchDenoiseConfig()));
    }

    @Test
    void nativeExtractorRefreshesStaleOrCorruptedBinaryByHash(@TempDir Path tempGameDir) throws Exception {
        NativeExtractor.ExtractedPaths first = NativeExtractor.extractAndDiscover(tempGameDir, null);
        assertTrue(Files.isRegularFile(first.linrtxSoPath()));
        assertTrue(Files.isRegularFile(first.libDir().resolve("libnvidia-ngx-dlssd.so.310.9.16")));
        assertTrue(Files.exists(first.libDir().resolve("libnvidia-ngx-dlssd.so")));
        long expectedSize = Files.size(first.linrtxSoPath());
        assertTrue(expectedSize > 10_000L);
        assertFalse(first.trueHdrSearchDirs().contains(".cache/uv"),
            () -> "Unexpected uv cache path in TrueHDR search dirs: " + first.trueHdrSearchDirs());

        // Overwrite extracted liblinrtx.so while keeping the current version stamp to simulate stale cache
        Files.writeString(first.linrtxSoPath(), "stale-broken-so");
        NativeExtractor.ExtractedPaths refreshed = NativeExtractor.extractAndDiscover(tempGameDir, null);
        assertEquals(expectedSize, Files.size(refreshed.linrtxSoPath()));
    }
}
