package dev.linrtx.mc.render;

import com.mojang.blaze3d.GpuFormat;
import com.mojang.blaze3d.opengl.GlTexture;
import com.mojang.blaze3d.pipeline.MainTarget;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.textures.GpuTexture;
import dev.linrtx.mc.compat.DistantHorizonsCompat;
import dev.linrtx.mc.compat.DistantHorizonsCompat.DhDepthSnapshot;
import dev.linrtx.mc.compat.IrisCompat;
import dev.linrtx.mc.config.LinRtxConfig;
import dev.linrtx.mc.config.LinRtxEnums.*;
import dev.linrtx.mc.natives.LinRtxNative;
import dev.linrtx.mc.natives.NativeExtractor;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.joml.Matrix4f;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL14;
import org.lwjgl.opengl.GL30;
import org.lwjgl.opengl.GL33;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.lang.foreign.MemorySegment;
import java.lang.reflect.Method;
import java.nio.file.Path;
import java.util.HashSet;
import java.util.Set;

/**
 * Central orchestrator for the 2-stage LinRTX native pipeline on Minecraft 26.2:
 * <ol>
 *   <li>Stage 1 (Pre-HUD World End): Extracts world color/depth/motion vectors,
 *       runs DLSS 4.5 + CAS sharpen, and writes the upscaled world back into full-res
 *       {@link RenderTarget} before Minecraft draws crisp full-res GUI/HUD.</li>
 *   <li>Stage 2 (Swap Present): Captures full-res composite + optional HUDless split,
 *       runs RTX HDR (TrueHDR / Compute Shader) and DLSS-G 2x/3x/4x Frame Generation,
 *       then presents interpolated + real frames cleanly via OpenGL swap or Vulkan HDR10.</li>
 * </ol>
 */
public final class LinRtxPipeline {
    private static final Logger LOGGER = LoggerFactory.getLogger("LinRTX-Pipeline");
    private static final LinRtxPipeline INSTANCE = new LinRtxPipeline();

    private final HaltonJitter jitter = new HaltonJitter();
    private final CameraStateTracker cameraTracker = new CameraStateTracker();
    private final TargetManager targets = new TargetManager();
    private final GlQuadPass quadPass = new GlQuadPass();
    private final DhDepthSnapshot dhSnapshot = new DhDepthSnapshot();

    private boolean nativeInitAttempted = false;
    private boolean nativeReady = false;
    private boolean shutdownDone = false;
    private long initWindowHandle = 0L;

    // Optimal scale cache
    private DlssMode cachedMode = null;
    private float cachedCustomScale = -1.0f;
    private int cachedDisplayW = 0;
    private int cachedDisplayH = 0;
    private int cachedRenderW = 0;
    private int cachedRenderH = 0;

    // Per-frame transient state
    private RenderTarget savedFullResTarget = null;
    private boolean activeWorldFrame = false;
    private boolean worldTargetSwapped = false;
    private boolean worldRenderedThisFrame = false;
    private boolean hudlessCapturedInUpscaled = false;
    private boolean depthMvecReadyThisFrame = false;
    private boolean colorIsHdrThisFrame = false;
    private boolean dhConsumedThisFrame = false;
    private boolean dhHasDataThisFrame = false;
    private boolean irisGuidesCapturedThisFrame = false;
    private boolean preTonemapDenoisedThisFrame = false;
    private boolean worldDepthSnapshotReadyThisFrame = false;
    private int preTonemapColortex5Id = 0;
    private int preTonemapBackupTexId = 0;
    private int preTonemapBackupW = 0;
    private int preTonemapBackupH = 0;
    private int activeRenderW = 0;
    private int activeRenderH = 0;
    private int activeDisplayW = 0;
    private int activeDisplayH = 0;
    private boolean activeMcReverseZ = true;
    private boolean activeMcZeroToOne = true;
    private int lastWorldDepthVersion = -1;

    private final Set<Integer> trackedSamplers = new HashSet<>();
    private float lastAppliedSamplerBias = 0.0f;

    private long lastFrameNs = 0L;
    private float lastFrameTimeMs = 16.666f;
    private int lastLoggedNativeErr = 0;

    private volatile LinRtxNative.StatusSnapshot statusSnapshot = LinRtxNative.StatusSnapshot.EMPTY;
    private volatile int currentDisplayW = 0;
    private volatile int currentDisplayH = 0;
    private volatile int currentRenderW = 0;
    private volatile int currentRenderH = 0;
    private volatile float currentMipBias = 0.0f;

    private LinRtxPipeline() {}

    public static LinRtxPipeline get() {
        return INSTANCE;
    }

    public boolean hasActiveWorldFrame() {
        return activeWorldFrame;
    }

    public void registerMipmappedSampler(int samplerId) {
        if (samplerId <= 0) {
            return;
        }
        trackedSamplers.add(samplerId);
        if (lastAppliedSamplerBias != 0.0f) {
            GL33.glSamplerParameterf(samplerId, GL14.GL_TEXTURE_LOD_BIAS, lastAppliedSamplerBias);
        }
    }

    public void unregisterMipmappedSampler(int samplerId) {
        if (samplerId > 0) {
            trackedSamplers.remove(samplerId);
        }
    }

    private void syncSamplerMipBias() {
        LinRtxConfig cfg = LinRtxConfig.get();
        float targetBias = (cfg.dlssMode != DlssMode.OFF && cfg.autoMipBias
                && currentDisplayW > 0 && currentRenderW > 0 && currentRenderW < currentDisplayW)
            ? Math.max(-3.0f, Math.min(0.0f, currentMipBias))
            : 0.0f;
        if (Math.abs(targetBias - lastAppliedSamplerBias) > 1e-3f) {
            for (int id : trackedSamplers) {
                GL33.glSamplerParameterf(id, GL14.GL_TEXTURE_LOD_BIAS, targetBias);
            }
            lastAppliedSamplerBias = targetBias;
        }
    }

    public float currentMipBias() {
        return currentMipBias;
    }

    public LinRtxNative.StatusSnapshot statusSnapshot() {
        return statusSnapshot;
    }

    public int currentRenderWidth() { return currentRenderW; }
    public int currentRenderHeight() { return currentRenderH; }
    public int currentDisplayWidth() { return currentDisplayW; }
    public int currentDisplayHeight() { return currentDisplayH; }

    public void requestHistoryReset() {
        jitter.reset();
        cameraTracker.requestReset();
    }

    private void ensureNativeInitialized(Minecraft mc, LinRtxConfig cfg) {
        if (nativeInitAttempted || shutdownDone) {
            return;
        }
        long winHandle = (mc.getWindow() != null) ? mc.getWindow().handle() : 0L;
        if (winHandle == 0L) {
            return;
        }
        nativeInitAttempted = true;
        initWindowHandle = winHandle;
        DistantHorizonsCompat.ensureRegistered();

        try {
            Path gameDir = mc.gameDirectory.toPath();
            NativeExtractor.ExtractedPaths paths = NativeExtractor.extractAndDiscover(
                gameDir, cfg.customTrueHdrDir, cfg.customNeuralDir);
            if (!LinRtxNative.loadLibrary(paths.linrtxSoPath())) {
                LOGGER.warn("Native FFM load failed: {}", LinRtxNative.getLoadError());
                return;
            }

            NativeWindowInfo nwi = extractNativeWindowInfo(winHandle);
            int rc = LinRtxNative.init(
                cfg.enableVulkanValidation,
                winHandle,
                nwi.displayPtr,
                nwi.windowHandle,
                nwi.isWayland,
                paths.libDir().toString(),
                paths.trueHdrSearchDirs(),
                paths.logDir().toString(),
                paths.neuralSearchDirs()
            );
            nativeReady = (rc == 0);
            statusSnapshot = LinRtxNative.getStatus();
            cachedMode = null; // Force NGX optimal scale query on next frame
            LOGGER.info("LinRTX native init rc={}, GPU='{}' ({}), DLSS={}, DLSS-RR={}, DLSS-G={}, TrueHDR={}, DLSS5-NR(NGX)={}",
                rc, statusSnapshot.gpuName(), statusSnapshot.gpuArchName(), statusSnapshot.dlssSupported(),
                statusSnapshot.dlssdSupported(), statusSnapshot.dlssgSupported(), statusSnapshot.trueHdrNgxLoaded(),
                statusSnapshot.dlssnrNgxLoaded());
        } catch (Throwable t) {
            LOGGER.error("LinRTX native initialization failed", t);
            nativeReady = false;
        }
    }

    private void resolveRenderResolution(LinRtxConfig cfg, int displayW, int displayH) {
        if (cfg.dlssMode == DlssMode.OFF) {
            cachedMode = DlssMode.OFF;
            cachedDisplayW = displayW;
            cachedDisplayH = displayH;
            cachedRenderW = displayW;
            cachedRenderH = displayH;
            currentDisplayW = displayW;
            currentDisplayH = displayH;
            currentRenderW = displayW;
            currentRenderH = displayH;
            currentMipBias = 0.0f;
            return;
        }

        if (cachedMode != cfg.dlssMode || cachedDisplayW != displayW || cachedDisplayH != displayH
                || (cfg.dlssMode == DlssMode.CUSTOM && Math.abs(cachedCustomScale - cfg.customScale) > 1e-4f)) {
            int rw = displayW;
            int rh = displayH;
            if (cfg.dlssMode == DlssMode.CUSTOM) {
                rw = clampEven(Math.round(displayW * cfg.customScale), displayW);
                rh = clampEven(Math.round(displayH * cfg.customScale), displayH);
            } else if (cfg.dlssMode != DlssMode.DLAA) {
                int[] opt = LinRtxNative.isLoaded()
                    ? LinRtxNative.getOptimalScale(cfg.dlssMode.nativeId, displayW, displayH)
                    : null;
                if (opt != null) {
                    rw = clampEven(opt[0], displayW);
                    rh = clampEven(opt[1], displayH);
                } else {
                    rw = clampEven(Math.round(displayW * cfg.dlssMode.defaultRatio), displayW);
                    rh = clampEven(Math.round(displayH * cfg.dlssMode.defaultRatio), displayH);
                }
            }
            if (rw != cachedRenderW || rh != cachedRenderH) {
                jitter.reset();
                cameraTracker.requestReset();
            }
            cachedMode = cfg.dlssMode;
            cachedCustomScale = cfg.customScale;
            cachedDisplayW = displayW;
            cachedDisplayH = displayH;
            cachedRenderW = rw;
            cachedRenderH = rh;
        }

        currentDisplayW = displayW;
        currentDisplayH = displayH;
        currentRenderW = cachedRenderW;
        currentRenderH = cachedRenderH;

        if (cfg.autoMipBias && cachedRenderW < displayW) {
            // Official DLSS LOD bias formula: log2(renderW / displayW) - 1.0
            currentMipBias = (float) (Math.log((double) cachedRenderW / (double) displayW) / Math.log(2.0) - 1.0);
        } else {
            currentMipBias = 0.0f;
        }
    }

    /**
     * Called immediately BEFORE {@code GameRenderer.renderLevel(DeltaTracker)}.
     * Optionally swaps {@code mainRenderTarget} to a scaled {@link MainTarget}.
     */
    public RenderTarget beginWorldFrame(
        Minecraft mc,
        RenderTarget currentMainTarget,
        boolean clearColorHasOverride,
        org.joml.Vector4f clearColorOverride
    ) {
        LinRtxConfig cfg = LinRtxConfig.get();
        ensureNativeInitialized(mc, cfg);

        activeWorldFrame = true;
        worldTargetSwapped = false;
        worldRenderedThisFrame = false;
        hudlessCapturedInUpscaled = false;
        depthMvecReadyThisFrame = false;
        dhConsumedThisFrame = false;
        dhHasDataThisFrame = false;
        irisGuidesCapturedThisFrame = false;
        preTonemapDenoisedThisFrame = false;
        worldDepthSnapshotReadyThisFrame = false;
        preTonemapColortex5Id = 0;
        if (currentMainTarget != null && currentMainTarget != targets.scaledWorldTarget()) {
            savedFullResTarget = currentMainTarget;
        }

        long now = System.nanoTime();
        if (lastFrameNs != 0L) {
            float dt = (now - lastFrameNs) / 1_000_000.0f;
            if (dt > 0.1f && dt < 1000.0f) {
                lastFrameTimeMs = dt;
            }
        }
        lastFrameNs = now;

        int displayW = Math.max(16, currentMainTarget.width);
        int displayH = Math.max(16, currentMainTarget.height);
        resolveRenderResolution(cfg, displayW, displayH);
        syncSamplerMipBias();

        activeDisplayW = displayW;
        activeDisplayH = displayH;
        activeRenderW = cachedRenderW;
        activeRenderH = cachedRenderH;

        preTonemapRrUsedThisFrame = false;
        Minecraft mcInst = Minecraft.getInstance();
        boolean curF1HideGui = IrisCompat.isPackInUse()
            && mcInst != null && mcInst.gui != null && mcInst.gui.hud != null
            && mcInst.gui.hud.isHidden();
        if (curF1HideGui != wasF1HideGuiIris) {
            wasF1HideGuiIris = curF1HideGui;
            cameraTracker.requestReset();
        }
        activeF1HideGui = curF1HideGui;

        if (cfg.dlssMode != DlssMode.OFF && !activeF1HideGui) {
            jitter.advance(activeRenderW, activeDisplayW);
        } else {
            jitter.reset();
        }

        RenderTarget fullRes = savedFullResTarget != null ? savedFullResTarget : currentMainTarget;
        int fullResDepthVer = IrisCompat.getDepthBufferVersion(fullRes);
        if (cfg.dlssMode != DlssMode.OFF && (activeRenderW != displayW || activeRenderH != displayH)) {
            MainTarget scaled = targets.ensureScaledWorldTarget(
                activeRenderW, activeRenderH, fullResDepthVer, lastWorldDepthVersion);
            if (scaled.getColorTexture() != null && scaled.getDepthTexture() != null) {
                RenderSystem.getDevice().createCommandEncoder().clearColorAndDepthTextures(
                    scaled.getColorTexture(),
                    clearColorHasOverride && clearColorOverride != null ? clearColorOverride : null,
                    scaled.getDepthTexture(),
                    0.0
                );
                worldTargetSwapped = true;
                lastWorldDepthVersion = IrisCompat.getDepthBufferVersion(scaled);
                return scaled;
            }
        }
        if (fullRes != targets.scaledWorldTarget()) {
            targets.destroyScaledTarget();
        }
        lastWorldDepthVersion = fullResDepthVer;
        return fullRes;
    }

    /**
     * Called from {@code GameRendererMixin} inside {@code ProjectionMatrixBuffer.getBuffer(Matrix4f)}
     * for the main world camera pass (never shadow pass).
     */
    public void onWorldProjection(
        Minecraft mc,
        Matrix4f projMat,
        CameraRenderState camState
    ) {
        if (IrisCompat.isRenderingShadowPass()) {
            return;
        }
        // Only track perspective main world camera
        if (Math.abs(projMat.m33()) > 1e-4f) {
            return;
        }

        LinRtxConfig cfg = LinRtxConfig.get();
        double camX = camState != null && camState.pos != null ? camState.pos.x : 0.0;
        double camY = camState != null && camState.pos != null ? camState.pos.y : 0.0;
        double camZ = camState != null && camState.pos != null ? camState.pos.z : 0.0;
        Matrix4f viewRot = (camState != null && camState.viewRotationMatrix != null)
            ? camState.viewRotationMatrix
            : new Matrix4f();
        float farPlane = camState != null && camState.depthFar > 1.0f ? camState.depthFar : 256.0f;
        float fovRad = (float) Math.toRadians(mc.options.fov().get());
        float aspect = (activeDisplayH > 0) ? ((float) activeDisplayW / (float) activeDisplayH) : (16.0f / 9.0f);

        cameraTracker.update(
            mc.level,
            camX, camY, camZ,
            projMat,
            viewRot,
            0.05f,
            farPlane,
            fovRad,
            aspect
        );

        activeMcReverseZ = IrisCompat.isReverseZActive();
        activeMcZeroToOne = RenderSystem.getDevice().getDeviceInfo().isZZeroToOne();

        if (cfg.dlssMode != DlssMode.OFF && activeRenderW > 0 && activeRenderH > 0 && !activeF1HideGui) {
            jitter.applyProjectionJitter(projMat, activeRenderW, activeRenderH);
        }
    }

    public static void onIrisCompositeStageBegin(String stageName, Object renderTargets) {
        INSTANCE.handleIrisCompositeStageBegin(stageName);
    }

    public static void onIrisCompositePassFinished(String stageName, String passName, Object renderTargets) {
        INSTANCE.handleIrisCompositePassFinished(stageName, passName, renderTargets);
    }

    public static void onIrisCompositeStageFinished(String stageName, Object renderTargets) {
        INSTANCE.restorePreTonemapBackupIfPending();
    }

    private void handleIrisCompositeStageBegin(String stageName) {
        if ("COMPOSITE".equalsIgnoreCase(stageName)) {
            preTonemapColortex5Id = 0;
        }
    }

    private void handleIrisCompositePassFinished(String stageName, String passName, Object renderTargets) {
        if (!activeWorldFrame || !"COMPOSITE".equalsIgnoreCase(stageName) || IrisCompat.isRenderingShadowPass()) {
            return;
        }
        if ("composite98".equals(passName)) {
            restorePreTonemapBackupIfPending();
            return;
        }
        if (!"composite19".equals(passName)) {
            return;
        }

        LinRtxConfig cfg = LinRtxConfig.get();
        IrisCompat.TargetInfo c5 = IrisCompat.inspectRenderTarget(renderTargets, 5);
        IrisCompat.TargetInfo c7 = IrisCompat.inspectRenderTarget(renderTargets, 7);
        IrisCompat.TargetInfo c8 = IrisCompat.inspectRenderTarget(renderTargets, 8);
        if (!c5.valid() || !c7.valid() || !c8.valid()) {
            return;
        }

        // Capture Y-flipped Vulkan top-left G-buffer guides before composite20+ mutates buffers
        if (nativeReady
                && cfg.gbufferGuideMode != GbufferGuideMode.NONE
                && cfg.gbufferGuideMode != GbufferGuideMode.SYNTHETIC
                && activeRenderW > 0 && activeRenderH > 0) {
            int albedoFmt = normalizeGuideColorFmt(c8.internalGlFormat());
            int normalsFmt = normalizeGuideNormalsFmt(c7.internalGlFormat());
            targets.ensureGuideTextures(activeRenderW, activeRenderH, albedoFmt, normalsFmt, 0, 0);
            quadPass.memoryBarrierAll();
            quadPass.copyToTexture(c8.mainTexGlId(), targets.diffuseAlbedoTex(), activeRenderW, activeRenderH, true);
            quadPass.copyToTexture(c7.mainTexGlId(), targets.normalsTex(), activeRenderW, activeRenderH, true);
            irisGuidesCapturedThisFrame = true;
        }

        // Pre-tonemap linear HDR DLSS-RR / SVGF on Camellia colortex5 before Bloom & AgX
        boolean wantPreRr = nativeReady
            && statusSnapshot.dlssdSupported()
            && (cfg.rrMode == RayReconstructionMode.ON
                || (cfg.rrMode == RayReconstructionMode.AUTO
                    && (cfg.dlssMode != DlssMode.OFF || cfg.denoiseMode != DenoiseMode.OFF)));
        if (nativeReady
                && cfg.preTonemapIrisDenoise
                && (wantPreRr || cfg.denoiseMode != DenoiseMode.OFF)
                && isSupportedPreTonemapFmt(c5.internalGlFormat())) {
            int irisDepthGl = IrisCompat.resolveRenderTargetsDepthGlId(renderTargets);
            if (irisDepthGl > 0) {
                int cw = c5.width();
                int ch = c5.height();
                int cfmt = c5.internalGlFormat();
                targets.ensurePreTonemapTextures(cw, ch, cfmt);
                quadPass.memoryBarrierAll();
                quadPass.copyImageDirect(c5.mainTexGlId(), targets.rawHdrBackupTex(), cw, ch);

                boolean hasDh = ensureDhSnapshotThisFrame(cfg);
                float jx = activeF1HideGui ? 0.0f : jitter.currentX();
                float jy = activeF1HideGui ? 0.0f : jitter.currentY();
                boolean useTopLeftVk = wantPreRr && cw == activeRenderW && ch == activeRenderH;

                int albTexId = 0;
                int albFmt = 0;
                int normTexId = 0;
                int normFmt = 0;

                if (useTopLeftVk) {
                    targets.ensureRenderTextures(cw, ch, GL30.GL_RGBA16F);
                    quadPass.runMotionVectorPass(
                        c5.mainTexGlId(),
                        irisDepthGl,
                        activeMcReverseZ,
                        activeMcZeroToOne,
                        jx,
                        jy,
                        dhSnapshot,
                        hasDh,
                        cameraTracker,
                        targets.colorInTex(),
                        targets.preDepthTex(),
                        targets.preMvecTex(),
                        cw,
                        ch
                    );
                    if (irisGuidesCapturedThisFrame) {
                        albTexId = targets.diffuseAlbedoTex();
                        albFmt = targets.guideDiffuseFmt();
                        normTexId = targets.normalsTex();
                        normFmt = targets.guideNormalsFmt();
                    }
                } else {
                    quadPass.runPreTonemapDepthMvPass(
                        irisDepthGl,
                        activeMcReverseZ,
                        activeMcZeroToOne,
                        jx,
                        jy,
                        dhSnapshot,
                        hasDh,
                        cameraTracker,
                        targets.preDepthTex(),
                        targets.preMvecTex(),
                        cw,
                        ch
                    );
                    if (cfg.gbufferGuideMode != GbufferGuideMode.NONE
                            && cfg.gbufferGuideMode != GbufferGuideMode.SYNTHETIC) {
                        albTexId = c8.mainTexGlId();
                        albFmt = c8.internalGlFormat();
                        normTexId = c7.mainTexGlId();
                        normFmt = c7.internalGlFormat();
                    }
                }

                MemorySegment dseg = populatePreTonemapSegment(
                    cfg, c5.mainTexGlId(), cw, ch, cfmt,
                    albTexId, albFmt, normTexId, normFmt,
                    useTopLeftVk, wantPreRr, jx, jy
                );

                int rc = LinRtxNative.evaluateDenoise(dseg);
                if (rc == 0) {
                    quadPass.memoryBarrierAll();
                    preTonemapColortex5Id = c5.mainTexGlId();
                    preTonemapBackupTexId = targets.rawHdrBackupTex();
                    preTonemapBackupW = cw;
                    preTonemapBackupH = ch;
                    preTonemapDenoisedThisFrame = true;
                    statusSnapshot = LinRtxNative.getStatus();
                    preTonemapRrUsedThisFrame = statusSnapshot.dlssdActive();
                }
            }
        }
    }

    private boolean preTonemapRrUsedThisFrame = false;
    private boolean wasF1HideGuiIris = false;
    private boolean activeF1HideGui = false;

    private MemorySegment populatePreTonemapSegment(
        LinRtxConfig cfg,
        int outColortex5GlId,
        int cw,
        int ch,
        int cfmt,
        int albTexId,
        int albFmt,
        int normTexId,
        int normFmt,
        boolean useTopLeftVk,
        boolean wantPreRr,
        float jx,
        float jy
    ) {
        int atrousPasses = (cfg.denoiseMode == DenoiseMode.SVGF_FAST)
            ? Math.min(2, cfg.denoiseAtrousPasses)
            : cfg.denoiseAtrousPasses;
        int effDenoiseMode = (cfg.denoiseMode != DenoiseMode.OFF)
            ? cfg.denoiseMode.nativeId
            : (wantPreRr ? DenoiseMode.SVGF_ATROUS.nativeId : DenoiseMode.OFF.nativeId);

        MemorySegment dseg = LinRtxNative.scratchDenoiseConfig();
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 4, cw);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 8, ch);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 12, cameraTracker.isResetPending() ? 1 : 0);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 16, targets.rawHdrBackupTex());
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 20, cfmt);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 24, outColortex5GlId);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 28, cfmt);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 32, targets.preDepthTex());
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 36, GL30.GL_R32F);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 40, targets.preMvecTex());
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 44, GL30.GL_RG16F);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 48, albTexId);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 52, albFmt);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 56, normTexId);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 60, normFmt);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 64, cfg.denoiseTemporalBlend);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 68, cfg.denoiseStrength);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 72, atrousPasses);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 76, 1); // preserveAlpha (sample count in .a)
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 80, albTexId > 0 ? 1 : 0);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 84, normTexId > 0 ? 1 : 0);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 88, cameraTracker.cameraNear());
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 92, cameraTracker.cameraFar());

        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 96,
            (useTopLeftVk && wantPreRr) ? cfg.rrMode.nativeId : RayReconstructionMode.OFF.nativeId);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 100, cfg.rrPreset.nativeId);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 104, effDenoiseMode);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 108, useTopLeftVk ? 1 : 0);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_INT, 112, activeF1HideGui ? 1 : 0);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 116, (float) cfg.f1AccumFadeSamples);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 120, jx);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 124, jy);
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 128, cameraTracker.cameraFovRadians());
        dseg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 132, cameraTracker.cameraAspect());
        LinRtxNative.putFloatArray(dseg, 136, cameraTracker.cameraPos(), 3);
        LinRtxNative.putFloatArray(dseg, 148, cameraTracker.cameraUp(), 3);
        LinRtxNative.putFloatArray(dseg, 160, cameraTracker.cameraRight(), 3);
        LinRtxNative.putFloatArray(dseg, 172, cameraTracker.cameraFwd(), 3);
        LinRtxNative.putFloatArray(dseg, 184, cameraTracker.viewToClipVk(), 16);
        LinRtxNative.putFloatArray(dseg, 248, cameraTracker.clipToViewVk(), 16);
        LinRtxNative.putFloatArray(dseg, 312, cameraTracker.clipToPrevClipVk(), 16);
        LinRtxNative.putFloatArray(dseg, 376, cameraTracker.prevClipToClipVk(), 16);
        LinRtxNative.putFloatArray(dseg, 440, cameraTracker.worldToViewRowMajor(), 16);
        return dseg;
    }

    private void restorePreTonemapBackupIfPending() {
        if (preTonemapColortex5Id != 0 && preTonemapBackupTexId != 0
                && preTonemapBackupW > 0 && preTonemapBackupH > 0) {
            quadPass.copyImageDirect(preTonemapBackupTexId, preTonemapColortex5Id, preTonemapBackupW, preTonemapBackupH);
        }
        preTonemapColortex5Id = 0;
    }

    private boolean ensureDhSnapshotThisFrame(LinRtxConfig cfg) {
        if (!cfg.combineDistantHorizonsDepth) {
            return false;
        }
        if (!dhConsumedThisFrame) {
            dhHasDataThisFrame = DistantHorizonsCompat.consumeFrameDepth(dhSnapshot);
            dhConsumedThisFrame = true;
        }
        return dhHasDataThisFrame;
    }

    /**
     * Snapshots world depth from {@code target} into an isolated {@code GL_R32F} texture
     * right before Minecraft clears the main depth buffer for always-on-top overlays.
     */
    public void captureWorldDepthSnapshot(RenderTarget target) {
        if (!activeWorldFrame || IrisCompat.isPackInUse() || target == null) {
            return;
        }
        int depthGl = glTexId(target.getDepthTexture());
        if (depthGl <= 0) {
            return;
        }
        int w = target.width;
        int h = target.height;
        if (w <= 0 || h <= 0) {
            return;
        }
        targets.ensureWorldDepthSnapshotTex(w, h);
        int snapshotGl = targets.worldDepthSnapshotTex();
        if (snapshotGl <= 0) {
            return;
        }
        quadPass.copyToTexture(depthGl, snapshotGl, w, h, false, false);
        worldDepthSnapshotReadyThisFrame = true;
    }

    /**
     * Called immediately after {@code LevelRenderer.render(...)} returns (before
     * {@code GameRenderer.renderLevel} clears depth for hand rendering and before GUI/Fog end).
     *
     * @return the full-resolution {@link RenderTarget} that must be restored on {@link GameRenderer}.
     */
    public RenderTarget endWorldFrame(RenderTarget currentMainTarget) {
        restorePreTonemapBackupIfPending();
        activeWorldFrame = false;
        LinRtxConfig cfg = LinRtxConfig.get();
        RenderTarget worldTarget = currentMainTarget;
        RenderTarget fullResTarget = (worldTargetSwapped && savedFullResTarget != null)
            ? savedFullResTarget
            : currentMainTarget;
        worldTargetSwapped = false;
        worldRenderedThisFrame = true;

        int worldColorGl = glTexId(worldTarget.getColorTexture());
        int fullResColorGl = glTexId(fullResTarget.getColorTexture());
        int worldDepthGl = (worldDepthSnapshotReadyThisFrame && !IrisCompat.isPackInUse() && targets.worldDepthSnapshotTex() > 0)
            ? targets.worldDepthSnapshotTex()
            : IrisCompat.resolveWorldDepthGlId(worldTarget);
        worldDepthSnapshotReadyThisFrame = false;
        colorIsHdrThisFrame = isHdrFormat(worldTarget.getColorTexture());

        int inputFmt = colorIsHdrThisFrame ? GL30.GL_RGBA16F : GL11.GL_RGBA8;
        int stage2Fmt = resolveStage2GlFormat(cfg, colorIsHdrThisFrame);

        boolean needDlss = cfg.dlssMode != DlssMode.OFF;
        boolean needStage1Denoise = (cfg.denoiseMode != DenoiseMode.OFF) && !preTonemapDenoisedThisFrame;
        boolean needNeural = cfg.neuralMode != NeuralEngineMode.OFF;
        boolean needStage1 = needDlss || needStage1Denoise || needNeural;
        boolean needFg = cfg.fgMode != FrameGenMode.OFF;
        boolean needMvPass = (needStage1 || needFg) && worldColorGl > 0 && worldDepthGl > 0;

        if (needMvPass) {
            targets.ensureRenderTextures(activeRenderW, activeRenderH, inputFmt);
            targets.ensureDisplayTextures(activeDisplayW, activeDisplayH, inputFmt, stage2Fmt);

            boolean hasDh = ensureDhSnapshotThisFrame(cfg);
            quadPass.runMotionVectorPass(
                worldColorGl,
                worldDepthGl,
                activeMcReverseZ,
                activeMcZeroToOne,
                activeF1HideGui ? 0.0f : jitter.currentX(),
                activeF1HideGui ? 0.0f : jitter.currentY(),
                dhSnapshot,
                hasDh,
                cameraTracker,
                targets.colorInTex(),
                targets.depthInTex(),
                targets.mvecInTex(),
                activeRenderW,
                activeRenderH
            );
            depthMvecReadyThisFrame = true;
        }

        boolean dlssEvaluatedOk = false;
        if (needStage1 && nativeReady && depthMvecReadyThisFrame && fullResColorGl > 0) {
            MemorySegment seg = populateFrameSegment(cfg, true, needStage1Denoise);
            int rc = LinRtxNative.evaluateDlss(seg);
            statusSnapshot = LinRtxNative.getStatus();
            if (rc >= 0) {
                // outUpscaledTex (Vulkan top-left) -> fullResTarget (OpenGL bottom-up for HUD rendering)
                quadPass.copyToTexture(targets.outUpscaledTex(), fullResColorGl, activeDisplayW, activeDisplayH, true, false);
                dlssEvaluatedOk = true;
                hudlessCapturedInUpscaled = true;
                lastLoggedNativeErr = 0;
            } else if (rc != lastLoggedNativeErr) {
                lastLoggedNativeErr = rc;
                LOGGER.warn("DLSS-SR / RR / Denoise evaluate returned {} ({})", rc, statusSnapshot.lastErrorMsg());
            }
        } else {
            if (preTonemapDenoisedThisFrame) {
                statusSnapshot = LinRtxNative.getStatus();
            }
            // Drain single-frame reset & DH snapshots cleanly when Stage-1 was skipped
            cameraTracker.checkAndClearReset();
            ensureDhSnapshotThisFrame(cfg);
        }

        // Fallback when DLSS is requested at scaled resolution but unavailable (e.g. non-RTX GPU)
        if (!dlssEvaluatedOk && worldTarget != fullResTarget && worldColorGl > 0 && fullResColorGl > 0) {
            quadPass.copyToTexture(worldColorGl, fullResColorGl, activeDisplayW, activeDisplayH, false, true);
        }

        // If HUDless split is requested for DLSS-G and DLSS-SR did not already generate outUpscaledTex,
        // capture fullResTarget now (pre-GUI) into hudlessInTex (Y-flipped).
        if (!hudlessCapturedInUpscaled && needFg && cfg.hudlessSplit && fullResColorGl > 0) {
            targets.ensureDisplayTextures(activeDisplayW, activeDisplayH, inputFmt, stage2Fmt);
            quadPass.copyToTexture(fullResColorGl, targets.hudlessInTex(), activeDisplayW, activeDisplayH, true);
        }

        return fullResTarget;
    }

    /**
     * Called from {@code GlSurfaceMixin.present()}.
     *
     * @return {@code true} if LinRTX handled swap/present itself (caller should cancel default glfwSwapBuffers),
     *         {@code false} for zero-overhead vanilla passthrough.
     */
    public boolean presentFrame(long windowHandle, int swapW, int swapH) {
        Minecraft mc = Minecraft.getInstance();
        LinRtxConfig cfg = LinRtxConfig.get();

        if (!worldRenderedThisFrame) {
            ensureNativeInitialized(mc, cfg);
        }

        boolean wantWorldFg = worldRenderedThisFrame && cfg.fgMode != FrameGenMode.OFF && depthMvecReadyThisFrame;
        boolean wantHdr = cfg.hdrEngine != HdrEngineMode.OFF;
        boolean wantVkSwap = cfg.presentBackend == PresentBackend.VULKAN_HDR;

        if (!nativeReady || (!wantWorldFg && !wantHdr && !wantVkSwap)) {
            worldRenderedThisFrame = false;
            return false;
        }

        RenderTarget mainTarget = mc.gameRenderer.mainRenderTarget();
        if (mainTarget == null) {
            worldRenderedThisFrame = false;
            return false;
        }
        int backbufferColorGl = glTexId(mainTarget.getColorTexture());
        if (backbufferColorGl <= 0) {
            worldRenderedThisFrame = false;
            return false;
        }

        int displayW = Math.max(16, mainTarget.width);
        int displayH = Math.max(16, mainTarget.height);
        activeDisplayW = displayW;
        activeDisplayH = displayH;
        if (activeRenderW <= 0 || activeRenderH <= 0) {
            activeRenderW = displayW;
            activeRenderH = displayH;
        }

        boolean hdrInput = worldRenderedThisFrame && colorIsHdrThisFrame;
        int inputFmt = hdrInput ? GL30.GL_RGBA16F : GL11.GL_RGBA8;
        int stage2Fmt = resolveStage2GlFormat(cfg, hdrInput);
        targets.ensureDisplayTextures(displayW, displayH, inputFmt, stage2Fmt);

        // Capture full final frame (World + GUI) into Vulkan top-left orientation
        quadPass.copyToTexture(backbufferColorGl, targets.backbufferInTex(), displayW, displayH, true);

        // Populate Stage-2 FrameConfig
        FrameGenMode savedFg = cfg.fgMode;
        if (!wantWorldFg) {
            // Temporarily omit FG on menu/loading frames while keeping RTX HDR active
            cfg.fgMode = FrameGenMode.OFF;
        }
        MemorySegment seg = populateFrameSegment(cfg, false, false);
        cfg.fgMode = savedFg;

        int genOrErr = LinRtxNative.evaluatePost(seg);
        statusSnapshot = LinRtxNative.getStatus();

        if (genOrErr < 0) {
            if (genOrErr != lastLoggedNativeErr) {
                lastLoggedNativeErr = genOrErr;
                LOGGER.warn("LinRTX Stage-2 failed ({}): {}", genOrErr, statusSnapshot.lastErrorMsg());
            }
            worldRenderedThisFrame = false;
            return false; // Let vanilla glfwSwapBuffers display backbuffer cleanly
        }
        lastLoggedNativeErr = 0;

        int genCount = wantWorldFg ? Math.max(0, genOrErr) : 0;
        boolean useVkSwap = wantVkSwap && statusSnapshot.vkSwapchainActive();

        if (useVkSwap) {
            for (int i = 0; i < genCount; i++) {
                LinRtxNative.vkPresentFrame(i);
            }
            LinRtxNative.vkPresentFrame(-1);
        } else {
            // Present interpolated frames chronologically BEFORE the new real frame
            for (int i = 0; i < genCount; i++) {
                if (i == 0 || LinRtxNative.fetchGeneratedFrame(i, targets.outInterpTex()) == 0) {
                    quadPass.presentToDefaultFramebuffer(targets.outInterpTex(), displayW, displayH, true);
                    GLFW.glfwSwapBuffers(windowHandle);
                }
            }

            int finalRealTex = (wantHdr && (statusSnapshot.trueHdrActive() || statusSnapshot.hdrShaderActive()))
                ? targets.outHdrOrFgTex()
                : targets.backbufferInTex();
            quadPass.presentToDefaultFramebuffer(finalRealTex, displayW, displayH, true);
            GLFW.glfwSwapBuffers(windowHandle);
        }

        worldRenderedThisFrame = false;
        return true;
    }

    public void onWindowResize(int w, int h) {
        if (w <= 0 || h <= 0) {
            return;
        }
        targets.destroyScaledTarget();
        if (nativeReady) {
            LinRtxNative.resizeDisplay(w, h);
            statusSnapshot = LinRtxNative.getStatus();
        }
        requestHistoryReset();
    }

    public void shutdown() {
        if (shutdownDone) {
            return;
        }
        shutdownDone = true;
        try {
            quadPass.destroy();
            targets.destroyAll();
            if (nativeReady) {
                LinRtxNative.shutdown();
                nativeReady = false;
            }
        } catch (Throwable t) {
            LOGGER.warn("Error during LinRTX shutdown: {}", t.toString());
        }
    }

    private MemorySegment populateFrameSegment(LinRtxConfig cfg, boolean consumeResetFlag, boolean allowStage1Denoise) {
        MemorySegment seg = LinRtxNative.scratchFrameConfig();
        boolean reset = consumeResetFlag
            ? cameraTracker.checkAndClearReset()
            : (worldRenderedThisFrame && !depthMvecReadyThisFrame);

        int hudlessTex = cfg.hudlessSplit
            ? (hudlessCapturedInUpscaled ? targets.outUpscaledTex() : targets.hudlessInTex())
            : 0;

        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 4, activeRenderW);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 8, activeRenderH);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 12, activeDisplayW);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 16, activeDisplayH);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 20, cfg.dlssMode.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 24, cfg.dlssPreset.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 28, cfg.fgMode.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 32, cfg.hdrEngine.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 36, cfg.hdrOutputMode.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 40, cfg.presentBackend.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 44, cfg.sharpness);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 48, activeF1HideGui ? 0.0f : jitter.currentX());
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 52, activeF1HideGui ? 0.0f : jitter.currentY());
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 56, (float) activeRenderW);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 60, (float) activeRenderH);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 64, lastFrameTimeMs);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 68, reset ? 1 : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 72, activeMcReverseZ ? 1 : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 76, colorIsHdrThisFrame ? 1 : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 80, (cfg.hudlessSplit && hudlessTex > 0) ? 1 : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 84, cfg.hdrContrast);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 88, cfg.hdrSaturation);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 92, cfg.hdrMiddleGray);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 96, cfg.hdrMaxLuminance);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 100, cfg.hdrDebanding ? 0 : 1);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 104, cfg.hdrHighlightBoost);

        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 108, targets.colorInTex());
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 112, targets.depthInTex());
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 116, targets.mvecInTex());
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 120, targets.outUpscaledTex());
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 124, hudlessTex);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 128, targets.backbufferInTex());
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 132, targets.outHdrOrFgTex());
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 136, targets.outInterpTex());
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 140, 1);

        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 144, cameraTracker.cameraNear());
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 148, cameraTracker.cameraFar());
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 152, cameraTracker.cameraFovRadians());
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 156, cameraTracker.cameraAspect());

        LinRtxNative.putFloatArray(seg, 160, cameraTracker.cameraPos(), 3);
        LinRtxNative.putFloatArray(seg, 172, cameraTracker.cameraUp(), 3);
        LinRtxNative.putFloatArray(seg, 184, cameraTracker.cameraRight(), 3);
        LinRtxNative.putFloatArray(seg, 196, cameraTracker.cameraFwd(), 3);
        LinRtxNative.putFloatArray(seg, 208, cameraTracker.viewToClipVk(), 16);
        LinRtxNative.putFloatArray(seg, 272, cameraTracker.clipToViewVk(), 16);
        LinRtxNative.putFloatArray(seg, 336, cameraTracker.clipToPrevClipVk(), 16);
        LinRtxNative.putFloatArray(seg, 400, cameraTracker.prevClipToClipVk(), 16);

        boolean supplyExplicitGuides = irisGuidesCapturedThisFrame
            && cfg.gbufferGuideMode != GbufferGuideMode.NONE
            && cfg.gbufferGuideMode != GbufferGuideMode.SYNTHETIC;
        boolean useGbufferGuides = cfg.gbufferGuideMode != GbufferGuideMode.NONE
            && (supplyExplicitGuides || cfg.gbufferGuideMode == GbufferGuideMode.SYNTHETIC);
        int atrousPasses = (cfg.denoiseMode == DenoiseMode.SVGF_FAST)
            ? Math.min(2, cfg.denoiseAtrousPasses)
            : cfg.denoiseAtrousPasses;

        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 464,
            allowStage1Denoise ? cfg.denoiseMode.nativeId : DenoiseMode.OFF.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 468,
            preTonemapDenoisedThisFrame ? RayReconstructionMode.OFF.nativeId : cfg.rrMode.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 472, cfg.rrPreset.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 476, useGbufferGuides ? 1 : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 480, supplyExplicitGuides ? 1 : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 484, cfg.denoiseTemporalBlend);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 488, cfg.denoiseStrength);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 492, atrousPasses);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 496, supplyExplicitGuides ? targets.diffuseAlbedoTex() : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 500, supplyExplicitGuides ? targets.guideDiffuseFmt() : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 504, supplyExplicitGuides ? targets.specularAlbedoTex() : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 508, supplyExplicitGuides ? targets.guideSpecFmt() : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 512, supplyExplicitGuides ? targets.normalsTex() : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 516, supplyExplicitGuides ? targets.guideNormalsFmt() : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 520, supplyExplicitGuides ? targets.roughnessTex() : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 524, supplyExplicitGuides ? targets.guideRoughFmt() : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 528,
            preTonemapRrUsedThisFrame ? LinRtxNative.LINRTX_FRAME_FLAG_PRETONEMAP_RR_ACTIVE : 0);
        LinRtxNative.putFloatArray(seg, 532, cameraTracker.worldToViewRowMajor(), 16);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 596, 0);

        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 600, cfg.neuralMode.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 604, cfg.neuralStyle.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 608, cfg.neuralPreset.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 612, cfg.neuralPlacement.nativeId);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 616, cfg.neuralUnlockAnyRtx ? 1 : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 620, cfg.neuralUseAutoMask ? 1 : 0);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 624, cfg.neuralWorkingScale);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 628, cfg.neuralIntensity);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 632, cfg.neuralLocalToneStrength);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 636, cfg.neuralLocalStructureStrength);
        seg.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, 640, cfg.neuralSkinStructureStrength);
        seg.set(java.lang.foreign.ValueLayout.JAVA_INT, 644, 0);
        return seg;
    }

    private static boolean isSupportedPreTonemapFmt(int glFmt) {
        return glFmt == GL30.GL_RGBA32F || glFmt == GL30.GL_RGBA16F;
    }

    private static int normalizeGuideColorFmt(int glFmt) {
        if (glFmt == GL30.GL_RGBA32F || glFmt == GL30.GL_RGBA16F || glFmt == GL11.GL_RGBA8) {
            return glFmt;
        }
        return GL11.GL_RGBA8;
    }

    private static int normalizeGuideNormalsFmt(int glFmt) {
        if (glFmt == GL30.GL_RGBA32F || glFmt == GL30.GL_RGBA16F) {
            return glFmt;
        }
        return GL30.GL_RGBA16F;
    }

    private static int resolveStage2GlFormat(LinRtxConfig cfg, boolean colorIsHdr) {
        if (cfg.hdrEngine != HdrEngineMode.OFF) {
            return switch (cfg.hdrOutputMode) {
                case PQ_BT2020 -> GL11.GL_RGB10_A2;
                case SCRGB_FP16 -> GL30.GL_RGBA16F;
                case SDR_BOOST -> GL11.GL_RGBA8;
            };
        }
        return colorIsHdr ? GL30.GL_RGBA16F : GL11.GL_RGBA8;
    }

    private static boolean isHdrFormat(GpuTexture tex) {
        return tex != null && tex.getFormat() == GpuFormat.RGBA16_FLOAT;
    }

    private static int glTexId(GpuTexture tex) {
        return (tex instanceof GlTexture glTex && !glTex.isClosed()) ? glTex.glId() : 0;
    }

    private static int clampEven(int val, int maxVal) {
        int c = Math.max(16, Math.min(maxVal, val));
        if (c < maxVal && (c & 1) != 0) {
            c = Math.min(maxVal, c + 1);
        }
        return c;
    }

    private record NativeWindowInfo(long displayPtr, long windowHandle, boolean isWayland) {}

    private static NativeWindowInfo extractNativeWindowInfo(long glfwWindow) {
        try {
            int platform = GLFW.glfwGetPlatform();
            if (platform == GLFW.GLFW_PLATFORM_WAYLAND) {
                Class<?> wl = Class.forName("org.lwjgl.glfw.GLFWNativeWayland");
                Method getDpy = wl.getMethod("glfwGetWaylandDisplay");
                Method getWin = wl.getMethod("glfwGetWaylandWindow", long.class);
                long dpy = ((Number) getDpy.invoke(null)).longValue();
                long win = ((Number) getWin.invoke(null, glfwWindow)).longValue();
                if (dpy != 0L && win != 0L) {
                    return new NativeWindowInfo(dpy, win, true);
                }
            }
        } catch (Throwable ignored) {
        }
        try {
            Class<?> x11 = Class.forName("org.lwjgl.glfw.GLFWNativeX11");
            Method getDpy = x11.getMethod("glfwGetX11Display");
            Method getWin = x11.getMethod("glfwGetX11Window", long.class);
            long dpy = ((Number) getDpy.invoke(null)).longValue();
            long win = ((Number) getWin.invoke(null, glfwWindow)).longValue();
            return new NativeWindowInfo(dpy, win, false);
        } catch (Throwable t) {
            LOGGER.debug("Could not extract native X11/Wayland window handles: {}", t.toString());
            return new NativeWindowInfo(0L, 0L, false);
        }
    }
}
