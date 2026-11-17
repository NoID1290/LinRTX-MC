package dev.linrtx.mc.compat;

import com.seibel.distanthorizons.api.DhApi;
import com.seibel.distanthorizons.api.enums.config.EDhApiDepthDirection;
import com.seibel.distanthorizons.api.enums.config.EDhApiDepthRange;
import com.seibel.distanthorizons.api.enums.rendering.EDhApiRenderPass;
import com.seibel.distanthorizons.api.interfaces.render.IDhApiRenderProxy;
import com.seibel.distanthorizons.api.methods.events.abstractEvents.DhApiAfterRenderEvent;
import com.seibel.distanthorizons.api.methods.events.abstractEvents.DhApiBeforeRenderPassEvent;
import com.seibel.distanthorizons.api.methods.events.sharedParameterObjects.DhApiEventParam;
import com.seibel.distanthorizons.api.methods.events.sharedParameterObjects.DhApiRenderParam;
import com.seibel.distanthorizons.api.objects.DhApiResult;
import net.fabricmc.loader.api.FabricLoader;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Runtime-safe compatibility bridge for Distant Horizons (3.x on MC 26.2).
 *
 * <p>Captures DH LOD depth texture ID, depth conventions, and near/far clip
 * planes each world frame so {@link dev.linrtx.mc.render.GlQuadPass} can
 * merge DH LOD depth with Minecraft depth in linear view space for crisp
 * distant motion vectors and DLSS disocclusion.
 */
public final class DistantHorizonsCompat {
    private static final Logger LOGGER = LoggerFactory.getLogger("LinRTX-DH");
    private static final boolean DH_LOADED = FabricLoader.getInstance().isModLoaded("distanthorizons");

    private static volatile boolean eventsRegistered = false;

    public static final class DhDepthSnapshot {
        public boolean valid;
        public int depthGlId;
        public float nearClip;
        public float farClip;
        public boolean reverseZ;
        public boolean negOneToPosOne;
    }

    private DistantHorizonsCompat() {}

    public static boolean isLoaded() {
        return DH_LOADED;
    }

    public static void ensureRegistered() {
        if (DH_LOADED && !eventsRegistered) {
            eventsRegistered = true;
            DhBridge.registerEvents();
        }
    }

    /**
     * Returns a snapshot of DH depth parameters captured during this frame, and
     * clears the per-frame validity flag so stale LOD depth isn't reused across menus.
     */
    public static boolean consumeFrameDepth(DhDepthSnapshot out) {
        if (!DH_LOADED) {
            out.valid = false;
            return false;
        }
        return DhBridge.consumeFrameDepth(out);
    }

    private static final class DhBridge {
        private static int capturedDepthGlId = 0;
        private static float capturedNear = 16.0f;
        private static float capturedFar = 16384.0f;
        private static boolean capturedReverseZ = true;
        private static boolean capturedNegOneToPosOne = false;
        private static boolean renderedThisFrame = false;

        static void registerEvents() {
            try {
                DhApi.events.bind(DhApiBeforeRenderPassEvent.class, new DhApiBeforeRenderPassEvent() {
                    @Override
                    public void beforeRender(DhApiEventParam<DhApiRenderParam> event) {
                        DhApiRenderParam param = event.value;
                        if (param == null) {
                            return;
                        }
                        if (param.renderPass == EDhApiRenderPass.OPAQUE
                                || param.renderPass == EDhApiRenderPass.OPAQUE_AND_TRANSPARENT) {
                            if (param.nearClipPlane > 0.0f && param.farClipPlane > param.nearClipPlane) {
                                capturedNear = param.nearClipPlane;
                                capturedFar = param.farClipPlane;
                            }
                            renderedThisFrame = true;
                        }
                    }
                });

                DhApi.events.bind(DhApiAfterRenderEvent.class, new DhApiAfterRenderEvent() {
                    @Override
                    public void afterRender(DhApiEventParam<Void> event) {
                        IDhApiRenderProxy proxy = DhApi.Delayed.renderProxy;
                        if (proxy == null) {
                            return;
                        }
                        try {
                            DhApiResult<Integer> depthRes = proxy.getDhDepthTextureGlId();
                            if (depthRes != null && depthRes.success && depthRes.payload != null && depthRes.payload > 0) {
                                capturedDepthGlId = depthRes.payload;
                                capturedReverseZ = proxy.getDepthDirection() == EDhApiDepthDirection.REVERSE_Z;
                                capturedNegOneToPosOne = proxy.getDepthRange() == EDhApiDepthRange.NEG_ONE_TO_POS_ONE;
                                renderedThisFrame = true;
                            }
                        } catch (Throwable t) {
                            LOGGER.debug("DH render proxy query error: {}", t.toString());
                        }
                    }
                });
                LOGGER.info("Registered Distant Horizons depth capture hooks.");
            } catch (Throwable t) {
                LOGGER.warn("Failed to bind Distant Horizons API events: {}", t.toString());
            }
        }

        static boolean consumeFrameDepth(DhDepthSnapshot out) {
            if (!renderedThisFrame || capturedDepthGlId <= 0) {
                out.valid = false;
                return false;
            }
            out.valid = true;
            out.depthGlId = capturedDepthGlId;
            out.nearClip = capturedNear;
            out.farClip = capturedFar;
            out.reverseZ = capturedReverseZ;
            out.negOneToPosOne = capturedNegOneToPosOne;
            renderedThisFrame = false;
            return true;
        }
    }
}
