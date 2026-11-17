package dev.linrtx.mc.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.mojang.blaze3d.buffers.GpuBufferSlice;
import com.mojang.blaze3d.pipeline.RenderTarget;
import dev.linrtx.mc.render.LinRtxPipeline;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.ProjectionMatrixBuffer;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.joml.Matrix4f;
import org.joml.Vector4f;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Mutable;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Hooks Minecraft 26.2's {@link GameRenderer} at priority 900 (running before Sodium's
 * priority 1000 mixin) to:
 * <ol>
 *   <li>Swap {@code mainRenderTarget} to a scaled target prior to {@code renderLevel}.</li>
 *   <li>Capture unjittered projection matrices and apply Halton subpixel jitter before
 *       Sodium, Iris, and Distant Horizons read the level projection matrix.</li>
 *   <li>Run Stage-1 motion-vector extraction + DLSS 4.5 upscaling cleanly before
 *       {@code FogRenderer.endFrame()} and GUI rendering.</li>
 * </ol>
 */
@Mixin(value = GameRenderer.class, priority = 900)
public abstract class GameRendererMixin {
    @Mutable
    @Shadow
    @Final
    private RenderTarget mainRenderTarget;

    @Shadow
    @Final
    private Minecraft minecraft;

    @Shadow
    @Final
    private GameRenderState gameRenderState;

    @Inject(
        method = "render(Lnet/minecraft/client/DeltaTracker;Z)V",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/GameRenderer;renderLevel(Lnet/minecraft/client/DeltaTracker;)V"
        )
    )
    private void linrtx$beforeRenderLevel(DeltaTracker deltaTracker, boolean renderLevel, CallbackInfo ci) {
        Vector4f clearOverride = (this.gameRenderState != null && this.gameRenderState.guiRenderState != null)
            ? this.gameRenderState.guiRenderState.clearColorOverride
            : null;
        this.mainRenderTarget = LinRtxPipeline.get().beginWorldFrame(
            this.minecraft,
            this.mainRenderTarget,
            clearOverride != null,
            clearOverride
        );
    }

    @WrapOperation(
        method = "renderLevel(Lnet/minecraft/client/DeltaTracker;)V",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/ProjectionMatrixBuffer;getBuffer(Lorg/joml/Matrix4f;)Lcom/mojang/blaze3d/buffers/GpuBufferSlice;"
        )
    )
    private GpuBufferSlice linrtx$onWorldProjection(
        ProjectionMatrixBuffer instance,
        Matrix4f projectionMatrix,
        Operation<GpuBufferSlice> original
    ) {
        CameraRenderState camState = (this.gameRenderState != null && this.gameRenderState.levelRenderState != null)
            ? this.gameRenderState.levelRenderState.cameraRenderState
            : null;
        LinRtxPipeline.get().onWorldProjection(this.minecraft, projectionMatrix, camState);
        return original.call(instance, projectionMatrix);
    }

    @Inject(
        method = "renderLevel(Lnet/minecraft/client/DeltaTracker;)V",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/LevelRenderer;render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;Lnet/minecraft/client/DeltaTracker;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lorg/joml/Matrix4fc;Lcom/mojang/blaze3d/buffers/GpuBufferSlice;Lorg/joml/Vector4f;Z)V",
            shift = At.Shift.AFTER
        )
    )
    private void linrtx$afterLevelRender(DeltaTracker deltaTracker, CallbackInfo ci) {
        if (LinRtxPipeline.get().hasActiveWorldFrame()) {
            this.mainRenderTarget = LinRtxPipeline.get().endWorldFrame(this.mainRenderTarget);
        }
    }

    @Inject(
        method = "render(Lnet/minecraft/client/DeltaTracker;Z)V",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/fog/FogRenderer;endFrame()V"
        )
    )
    private void linrtx$beforeFogEndFrame(DeltaTracker deltaTracker, boolean renderLevel, CallbackInfo ci) {
        if (LinRtxPipeline.get().hasActiveWorldFrame()) {
            this.mainRenderTarget = LinRtxPipeline.get().endWorldFrame(this.mainRenderTarget);
        }
    }

    @Inject(method = "resize(II)V", at = @At("TAIL"))
    private void linrtx$onResize(int width, int height, CallbackInfo ci) {
        LinRtxPipeline.get().onWindowResize(width, height);
    }
}
