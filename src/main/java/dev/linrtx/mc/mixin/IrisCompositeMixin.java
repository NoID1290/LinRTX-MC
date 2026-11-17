package dev.linrtx.mc.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.linrtx.mc.render.LinRtxPipeline;
import net.irisshaders.iris.pipeline.CompositePass;
import net.irisshaders.iris.pipeline.CompositeRenderer;
import net.irisshaders.iris.targets.RenderTargets;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Hooks Iris {@link CompositeRenderer#renderAll()} (gated by {@link LinRtxMixinPlugin})
 * to capture G-buffer guides (normals/albedo) and optionally run SVGF denoising on linear
 * path-traced HDR buffers (such as Camellia's {@code colortex5}) prior to Bloom and AgX
 * tonemapping.
 */
@Mixin(value = CompositeRenderer.class, remap = false)
public abstract class IrisCompositeMixin {
    @Shadow
    @Final
    private RenderTargets renderTargets;

    @Shadow
    @Final
    private CompositePass compositePass;

    @Unique
    private String linrtx$activePassName = "";

    @Inject(method = "renderAll", at = @At("HEAD"))
    private void linrtx$onStageBegin(CallbackInfo ci) {
        linrtx$activePassName = "";
        if (compositePass != null && renderTargets != null) {
            LinRtxPipeline.onIrisCompositeStageBegin(compositePass.name(), renderTargets);
        }
    }

    @WrapOperation(
        method = "renderAll",
        at = @At(
            value = "INVOKE",
            target = "Lnet/irisshaders/iris/gl/GLDebug;pushGroup(ILjava/lang/String;)V",
            ordinal = 1
        ),
        require = 0
    )
    private void linrtx$capturePassName(int id, String name, Operation<Void> original) {
        linrtx$activePassName = (name != null) ? name : "";
        original.call(id, name);
    }

    @Inject(
        method = "renderAll",
        at = {
            @At(
                value = "INVOKE",
                target = "Lnet/irisshaders/iris/gl/GLDebug;popGroup()V",
                ordinal = 0,
                shift = At.Shift.AFTER
            ),
            @At(
                value = "INVOKE",
                target = "Lnet/irisshaders/iris/gl/GLDebug;popGroup()V",
                ordinal = 1,
                shift = At.Shift.AFTER
            )
        },
        require = 0
    )
    private void linrtx$onPassFinished(CallbackInfo ci) {
        if (compositePass != null && renderTargets != null && !linrtx$activePassName.isEmpty()) {
            LinRtxPipeline.onIrisCompositePassFinished(compositePass.name(), linrtx$activePassName, renderTargets);
        }
    }

    @Inject(method = "renderAll", at = @At("RETURN"))
    private void linrtx$onStageFinished(CallbackInfo ci) {
        if (compositePass != null && renderTargets != null) {
            LinRtxPipeline.onIrisCompositeStageFinished(compositePass.name(), renderTargets);
        }
        linrtx$activePassName = "";
    }
}
