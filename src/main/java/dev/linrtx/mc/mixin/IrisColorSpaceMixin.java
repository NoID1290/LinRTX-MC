package dev.linrtx.mc.mixin;

import com.mojang.blaze3d.opengl.GlTexture;
import dev.linrtx.mc.render.LinRtxPipeline;
import net.irisshaders.iris.pathways.colorspace.ColorSpace;
import net.irisshaders.iris.pathways.colorspace.ColorSpaceFragmentConverter;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Ensures Iris {@link ColorSpaceFragmentConverter} (gated by {@link LinRtxMixinPlugin})
 * sizes its pass to the full display target rather than the scaled world target, since
 * {@code finalizeGameRendering()} processes the restored full-res framebuffer after DLSS upscaling.
 */
@Mixin(value = ColorSpaceFragmentConverter.class, remap = false)
public abstract class IrisColorSpaceMixin {
    @Shadow
    private int width;

    @Shadow
    private int height;

    @Shadow
    private ColorSpace colorSpace;

    @Shadow
    public abstract void rebuildProgram(int width, int height, ColorSpace colorSpace);

    @ModifyVariable(
        method = "rebuildProgram(IILnet/irisshaders/iris/pathways/colorspace/ColorSpace;)V",
        at = @At("HEAD"),
        argsOnly = true,
        ordinal = 0
    )
    private int linrtx$useDisplayWidth(int w) {
        LinRtxPipeline p = LinRtxPipeline.get();
        if (p.hasActiveWorldFrame() && p.currentDisplayWidth() > 0 && w == p.currentRenderWidth()) {
            return p.currentDisplayWidth();
        }
        return w;
    }

    @ModifyVariable(
        method = "rebuildProgram(IILnet/irisshaders/iris/pathways/colorspace/ColorSpace;)V",
        at = @At("HEAD"),
        argsOnly = true,
        ordinal = 1
    )
    private int linrtx$useDisplayHeight(int h) {
        LinRtxPipeline p = LinRtxPipeline.get();
        if (p.hasActiveWorldFrame() && p.currentDisplayHeight() > 0 && h == p.currentRenderHeight()) {
            return p.currentDisplayHeight();
        }
        return h;
    }

    @Inject(method = "process(Lcom/mojang/blaze3d/opengl/GlTexture;)V", at = @At("HEAD"))
    private void linrtx$syncColorSpaceSize(GlTexture target, CallbackInfo ci) {
        if (this.colorSpace == ColorSpace.SRGB || target == null) {
            return;
        }
        int tw = target.getWidth(0);
        int th = target.getHeight(0);
        if (tw > 0 && th > 0 && (this.width != tw || this.height != th)) {
            this.rebuildProgram(tw, th, this.colorSpace);
        }
    }
}
