package dev.linrtx.mc.compat;

import com.mojang.blaze3d.opengl.GlTexture;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.textures.GpuTexture;
import net.fabricmc.loader.api.FabricLoader;
import net.irisshaders.iris.Iris;
import net.irisshaders.iris.api.v0.IrisApi;
import net.irisshaders.iris.pipeline.IrisRenderingPipeline;
import net.irisshaders.iris.pipeline.WorldRenderingPipeline;
import net.irisshaders.iris.targets.Blaze3dRenderTargetExt;
import net.irisshaders.iris.targets.RenderTargets;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL30;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.lang.reflect.Field;

/**
 * Runtime-safe compatibility bridge for Iris Shaders (Iris 1.11.x on MC 26.2).
 *
 * <p>All direct references to Iris classes live inside {@link IrisBridge} so
 * this class safely loads even when Iris is absent at runtime.
 */
public final class IrisCompat {
    private static final Logger LOGGER = LoggerFactory.getLogger("LinRTX-Iris");
    private static final boolean IRIS_LOADED = FabricLoader.getInstance().isModLoaded("iris");

    private IrisCompat() {}

    public record TargetInfo(
        int mainTexGlId,
        int altTexGlId,
        int internalGlFormat,
        int width,
        int height
    ) {
        public static final TargetInfo INVALID = new TargetInfo(0, 0, 0, 0, 0);

        public boolean valid() {
            return mainTexGlId > 0 && width > 0 && height > 0;
        }
    }

    public static boolean isLoaded() {
        return IRIS_LOADED;
    }

    public static String getCurrentPackName() {
        return IRIS_LOADED ? IrisBridge.getCurrentPackName() : "";
    }

    public static TargetInfo inspectRenderTarget(Object renderTargetsObj, int colorIndex) {
        if (!IRIS_LOADED) return TargetInfo.INVALID;
        return IrisBridge.inspectRenderTarget(renderTargetsObj, colorIndex);
    }

    public static int resolveRenderTargetsDepthGlId(Object renderTargetsObj) {
        if (!IRIS_LOADED) return 0;
        return IrisBridge.getDepthFromTargets(renderTargetsObj);
    }

    public static boolean isPackInUse() {
        return IRIS_LOADED && IrisBridge.isPackInUse();
    }

    public static boolean isRenderingShadowPass() {
        return IRIS_LOADED && IrisBridge.isRenderingShadowPass();
    }

    /**
     * Returns whether the current world pass uses Reverse-Z ([1 near, 0 far]).
     * Vanilla MC 26.2 and Iris 1.11.4 main camera pass both use Reverse-Z.
     */
    public static boolean isReverseZActive() {
        if (!IRIS_LOADED || !IrisBridge.isPackInUse()) {
            return true;
        }
        return IrisBridge.isReverseZDuringShaders();
    }

    /**
     * Returns Iris's internal depth buffer version for a {@link RenderTarget}, or {@code -1}
     * when Iris is absent or the target does not implement {@code Blaze3dRenderTargetExt}.
     */
    public static int getDepthBufferVersion(RenderTarget target) {
        if (!IRIS_LOADED || target == null) {
            return -1;
        }
        return IrisBridge.getDepthBufferVersion(target);
    }

    /**
     * Returns Iris's internal color buffer version for a {@link RenderTarget}, or {@code -1}
     * when Iris is absent.
     */
    public static int getColorBufferVersion(RenderTarget target) {
        if (!IRIS_LOADED || target == null) {
            return -1;
        }
        return IrisBridge.getColorBufferVersion(target);
    }

    /**
     * Resolves the OpenGL texture ID for the world depth buffer. When an Iris
     * shaderpack is active, Iris's {@link RenderTargets#getDepthTexture()} is
     * checked first, falling back to {@code worldTarget.getDepthTexture()}.
     */
    public static int resolveWorldDepthGlId(RenderTarget worldTarget) {
        if (IRIS_LOADED && IrisBridge.isPackInUse()) {
            int irisDepth = IrisBridge.getIrisDepthGlId();
            if (irisDepth > 0) {
                return irisDepth;
            }
        }
        if (worldTarget != null) {
            GpuTexture tex = worldTarget.getDepthTexture();
            if (tex instanceof GlTexture glTex && !glTex.isClosed()) {
                return glTex.glId();
            }
        }
        return 0;
    }

    /**
     * Triggered only when Iris is confirmed present by FabricLoader.
     */
    private static final class IrisBridge {
        private static final Field RENDER_TARGETS_FIELD = findRenderTargetsField();

        private static Field findRenderTargetsField() {
            try {
                Field f = IrisRenderingPipeline.class.getDeclaredField("renderTargets");
                f.setAccessible(true);
                return f;
            } catch (Throwable t) {
                LOGGER.warn("Could not reflect IrisRenderingPipeline.renderTargets: {}", t.toString());
                return null;
            }
        }

        static boolean isPackInUse() {
            try {
                return IrisApi.getInstance().isShaderPackInUse();
            } catch (Throwable t) {
                return false;
            }
        }

        static boolean isRenderingShadowPass() {
            try {
                return IrisApi.getInstance().isRenderingShadowPass();
            } catch (Throwable t) {
                return false;
            }
        }

        static boolean isReverseZDuringShaders() {
            try {
                return IrisApi.getInstance().isReverseZDuringShaders();
            } catch (Throwable t) {
                return true;
            }
        }

        static int getDepthBufferVersion(RenderTarget target) {
            if (target instanceof Blaze3dRenderTargetExt ext) {
                return ext.iris$getDepthBufferVersion();
            }
            return -1;
        }

        static int getColorBufferVersion(RenderTarget target) {
            if (target instanceof Blaze3dRenderTargetExt ext) {
                return ext.iris$getColorBufferVersion();
            }
            return -1;
        }

        static String getCurrentPackName() {
            try {
                String name = Iris.getCurrentPackName();
                return name != null ? name : "";
            } catch (Throwable t) {
                return "";
            }
        }

        static TargetInfo inspectRenderTarget(Object renderTargetsObj, int colorIndex) {
            try {
                if (!(renderTargetsObj instanceof RenderTargets targets)) {
                    return TargetInfo.INVALID;
                }
                if (colorIndex < 0 || colorIndex >= targets.getRenderTargetCount()) {
                    return TargetInfo.INVALID;
                }
                net.irisshaders.iris.targets.RenderTarget rt = targets.get(colorIndex);
                if (rt == null || rt.getMainTexture() <= 0 || rt.getWidth() <= 0 || rt.getHeight() <= 0) {
                    return TargetInfo.INVALID;
                }
                int glFmt = normalizeGlColorFormat(rt.getInternalFormat().getGlFormat());
                return new TargetInfo(rt.getMainTexture(), rt.getAltTexture(), glFmt, rt.getWidth(), rt.getHeight());
            } catch (Throwable t) {
                return TargetInfo.INVALID;
            }
        }

        static int getDepthFromTargets(Object renderTargetsObj) {
            try {
                if (renderTargetsObj instanceof RenderTargets targets) {
                    GpuTexture depthTex = targets.getDepthTexture();
                    if (depthTex instanceof GlTexture glTex && !glTex.isClosed()) {
                        return glTex.glId();
                    }
                }
            } catch (Throwable t) {
                LOGGER.debug("Failed reading depth from Iris RenderTargets: {}", t.toString());
            }
            return getIrisDepthGlId();
        }

        private static int normalizeGlColorFormat(int glFmt) {
            if (glFmt == GL11.GL_RGBA) {
                return GL11.GL_RGBA8;
            }
            if (glFmt == GL30.GL_RGB16F) {
                return GL30.GL_RGBA16F;
            }
            return glFmt;
        }

        static int getIrisDepthGlId() {
            if (RENDER_TARGETS_FIELD == null) {
                return 0;
            }
            try {
                WorldRenderingPipeline pipeline = Iris.getPipelineManager().getPipelineNullable();
                if (!(pipeline instanceof IrisRenderingPipeline irisPipeline)) {
                    return 0;
                }
                Object obj = RENDER_TARGETS_FIELD.get(irisPipeline);
                if (obj instanceof RenderTargets targets) {
                    GpuTexture depthTex = targets.getDepthTexture();
                    if (depthTex instanceof GlTexture glTex && !glTex.isClosed()) {
                        return glTex.glId();
                    }
                }
            } catch (Throwable t) {
                LOGGER.debug("Failed to read Iris depth texture: {}", t.toString());
            }
            return 0;
        }
    }
}
