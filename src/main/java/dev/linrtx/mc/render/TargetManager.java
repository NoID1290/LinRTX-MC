package dev.linrtx.mc.render;

import com.mojang.blaze3d.opengl.GlStateManager;
import com.mojang.blaze3d.pipeline.MainTarget;
import dev.linrtx.mc.compat.IrisCompat;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL12;
import org.lwjgl.opengl.GL30;
import org.lwjgl.opengl.GL42;

import java.nio.ByteBuffer;

/**
 * Owns scaled low-res Minecraft {@link MainTarget} plus sized OpenGL textures
 * formatted specifically for NVIDIA NGX Vulkan external-memory interop.
 */
public final class TargetManager {
    private MainTarget scaledWorldTarget;
    private int scaledW = 0;
    private int scaledH = 0;

    // Render-resolution inputs for Stage 1 / Stage 2 (Vulkan top-left orientation)
    private int colorInTex = 0;
    private int depthInTex = 0;
    private int mvecInTex = 0;
    private int worldDepthSnapshotTex = 0;
    private int texRenderW = 0;
    private int texRenderH = 0;
    private int snapshotW = 0;
    private int snapshotH = 0;
    private int renderColorGlFmt = 0;

    // Display-resolution outputs/inputs (Vulkan top-left orientation)
    private int outUpscaledTex = 0;
    private int hudlessInTex = 0;
    private int backbufferInTex = 0;
    private int outHdrOrFgTex = 0;
    private int outInterpTex = 0;
    private int texDisplayW = 0;
    private int texDisplayH = 0;
    private int displayInputGlFmt = 0;
    private int stage2GlFmt = 0;

    // Optional G-buffer guides (low-res Vulkan top-left orientation for DLSS-RR / SVGF)
    private int diffuseAlbedoTex = 0;
    private int specularAlbedoTex = 0;
    private int normalsTex = 0;
    private int roughnessTex = 0;
    private int guideW = 0;
    private int guideH = 0;
    private int guideDiffuseFmt = 0;
    private int guideSpecFmt = 0;
    private int guideNormalsFmt = 0;
    private int guideRoughFmt = 0;

    // Pre-tonemap Iris denoiser targets (GL bottom-up orientation)
    private int preDepthTex = 0;
    private int preMvecTex = 0;
    private int rawHdrBackupTex = 0;
    private int preW = 0;
    private int preH = 0;
    private int preHdrFmt = 0;

    public MainTarget ensureScaledWorldTarget(int renderW, int renderH, int forbiddenDepthVer, int lastUsedDepthVer) {
        if (scaledWorldTarget == null) {
            scaledWorldTarget = new MainTarget(renderW, renderH);
            scaledW = scaledWorldTarget.width;
            scaledH = scaledWorldTarget.height;
        } else if (scaledW != renderW || scaledH != renderH) {
            // Reuse and resize existing MainTarget so Iris's iris$depthBufferVersion increments monotonically
            // instead of resetting to 0 on every resolution change.
            scaledWorldTarget.resize(renderW, renderH);
            scaledW = scaledWorldTarget.width;
            scaledH = scaledWorldTarget.height;
        }

        // Ensure Iris / Distant Horizons always observe a distinct depthBufferVersion across target swaps
        // (e.g. when a newly constructed MainTarget starts at version 0 matching a previous target).
        if (IrisCompat.isLoaded()) {
            for (int attempt = 0; attempt < 4; attempt++) {
                int ver = IrisCompat.getDepthBufferVersion(scaledWorldTarget);
                if (ver < 0 || (ver != forbiddenDepthVer && ver != lastUsedDepthVer)) {
                    break;
                }
                scaledWorldTarget.resize(renderW, renderH);
                scaledW = scaledWorldTarget.width;
                scaledH = scaledWorldTarget.height;
            }
        }
        return scaledWorldTarget;
    }

    public void destroyScaledTarget() {
        if (scaledWorldTarget != null) {
            try {
                scaledWorldTarget.destroyBuffers();
            } catch (Throwable ignored) {
            }
            scaledWorldTarget = null;
            scaledW = 0;
            scaledH = 0;
        }
    }

    public void ensureRenderTextures(int renderW, int renderH, int colorInternalFmt) {
        if (colorInTex != 0 && texRenderW == renderW && texRenderH == renderH && renderColorGlFmt == colorInternalFmt) {
            return;
        }
        destroyRenderTextures();
        colorInTex = allocTex2D(renderW, renderH, colorInternalFmt);
        depthInTex = allocTex2D(renderW, renderH, GL30.GL_R32F);
        mvecInTex = allocTex2D(renderW, renderH, GL30.GL_RG16F);
        texRenderW = renderW;
        texRenderH = renderH;
        renderColorGlFmt = colorInternalFmt;
    }

    public void ensureDisplayTextures(int displayW, int displayH, int inputColorFmt, int outputStage2Fmt) {
        if (outUpscaledTex != 0 && texDisplayW == displayW && texDisplayH == displayH
                && displayInputGlFmt == inputColorFmt && stage2GlFmt == outputStage2Fmt) {
            return;
        }
        destroyDisplayTextures();
        outUpscaledTex = allocTex2D(displayW, displayH, inputColorFmt);
        hudlessInTex = allocTex2D(displayW, displayH, inputColorFmt);
        backbufferInTex = allocTex2D(displayW, displayH, inputColorFmt);
        outHdrOrFgTex = allocTex2D(displayW, displayH, outputStage2Fmt);
        outInterpTex = allocTex2D(displayW, displayH, outputStage2Fmt);
        texDisplayW = displayW;
        texDisplayH = displayH;
        displayInputGlFmt = inputColorFmt;
        stage2GlFmt = outputStage2Fmt;
    }

    public void ensureGuideTextures(int w, int h, int diffuseFmt, int normalsFmt, int specFmt, int roughFmt) {
        if (guideW == w && guideH == h
                && guideDiffuseFmt == diffuseFmt
                && guideNormalsFmt == normalsFmt
                && guideSpecFmt == specFmt
                && guideRoughFmt == roughFmt) {
            return;
        }
        destroyGuideTextures();
        if (diffuseFmt != 0) diffuseAlbedoTex = allocTex2D(w, h, diffuseFmt);
        if (normalsFmt != 0) normalsTex = allocTex2D(w, h, normalsFmt);
        if (specFmt != 0) specularAlbedoTex = allocTex2D(w, h, specFmt);
        if (roughFmt != 0) roughnessTex = allocTex2D(w, h, roughFmt);
        guideW = w;
        guideH = h;
        guideDiffuseFmt = diffuseFmt;
        guideNormalsFmt = normalsFmt;
        guideSpecFmt = specFmt;
        guideRoughFmt = roughFmt;
    }

    public void ensurePreTonemapTextures(int w, int h, int hdrColorFmt) {
        if (preDepthTex != 0 && preW == w && preH == h && preHdrFmt == hdrColorFmt) {
            return;
        }
        destroyPreTonemapTextures();
        preDepthTex = allocTex2D(w, h, GL30.GL_R32F);
        preMvecTex = allocTex2D(w, h, GL30.GL_RG16F);
        rawHdrBackupTex = allocTex2D(w, h, hdrColorFmt);
        preW = w;
        preH = h;
        preHdrFmt = hdrColorFmt;
    }

    public void destroyAll() {
        destroyScaledTarget();
        destroyRenderTextures();
        destroyDisplayTextures();
        destroyGuideTextures();
        destroyPreTonemapTextures();
    }

    private void destroyGuideTextures() {
        diffuseAlbedoTex = deleteTex(diffuseAlbedoTex);
        specularAlbedoTex = deleteTex(specularAlbedoTex);
        normalsTex = deleteTex(normalsTex);
        roughnessTex = deleteTex(roughnessTex);
        guideW = 0;
        guideH = 0;
        guideDiffuseFmt = 0;
        guideSpecFmt = 0;
        guideNormalsFmt = 0;
        guideRoughFmt = 0;
    }

    private void destroyPreTonemapTextures() {
        preDepthTex = deleteTex(preDepthTex);
        preMvecTex = deleteTex(preMvecTex);
        rawHdrBackupTex = deleteTex(rawHdrBackupTex);
        preW = 0;
        preH = 0;
        preHdrFmt = 0;
    }

    private void destroyRenderTextures() {
        colorInTex = deleteTex(colorInTex);
        depthInTex = deleteTex(depthInTex);
        mvecInTex = deleteTex(mvecInTex);
        worldDepthSnapshotTex = deleteTex(worldDepthSnapshotTex);
        texRenderW = 0;
        texRenderH = 0;
        snapshotW = 0;
        snapshotH = 0;
    }

    public void ensureWorldDepthSnapshotTex(int w, int h) {
        if (w <= 0 || h <= 0) {
            return;
        }
        if (worldDepthSnapshotTex != 0 && snapshotW == w && snapshotH == h) {
            return;
        }
        worldDepthSnapshotTex = deleteTex(worldDepthSnapshotTex);
        worldDepthSnapshotTex = allocTex2D(w, h, org.lwjgl.opengl.GL30.GL_R32F);
        snapshotW = w;
        snapshotH = h;
    }

    private void destroyDisplayTextures() {
        outUpscaledTex = deleteTex(outUpscaledTex);
        hudlessInTex = deleteTex(hudlessInTex);
        backbufferInTex = deleteTex(backbufferInTex);
        outHdrOrFgTex = deleteTex(outHdrOrFgTex);
        outInterpTex = deleteTex(outInterpTex);
        texDisplayW = 0;
        texDisplayH = 0;
        stage2GlFmt = 0;
    }

    private static int allocTex2D(int w, int h, int internalFmt) {
        int prevTex = GL11.glGetInteger(GL11.GL_TEXTURE_BINDING_2D);
        int tex = GL11.glGenTextures();
        GL11.glBindTexture(GL11.GL_TEXTURE_2D, tex);
        GL11.glTexParameteri(GL11.GL_TEXTURE_2D, GL11.GL_TEXTURE_MIN_FILTER, GL11.GL_NEAREST);
        GL11.glTexParameteri(GL11.GL_TEXTURE_2D, GL11.GL_TEXTURE_MAG_FILTER, GL11.GL_NEAREST);
        GL11.glTexParameteri(GL11.GL_TEXTURE_2D, GL11.GL_TEXTURE_WRAP_S, GL12.GL_CLAMP_TO_EDGE);
        GL11.glTexParameteri(GL11.GL_TEXTURE_2D, GL11.GL_TEXTURE_WRAP_T, GL12.GL_CLAMP_TO_EDGE);
        GL42.glTexStorage2D(GL11.GL_TEXTURE_2D, 1, internalFmt, w, h);
        GL11.glBindTexture(GL11.GL_TEXTURE_2D, prevTex);
        return tex;
    }

    private static int deleteTex(int tex) {
        if (tex != 0) {
            GL11.glDeleteTextures(tex);
        }
        return 0;
    }

    public int colorInTex() { return colorInTex; }
    public int depthInTex() { return depthInTex; }
    public int mvecInTex() { return mvecInTex; }
    public int outUpscaledTex() { return outUpscaledTex; }
    public int hudlessInTex() { return hudlessInTex; }
    public int backbufferInTex() { return backbufferInTex; }
    public int outHdrOrFgTex() { return outHdrOrFgTex; }
    public int outInterpTex() { return outInterpTex; }
    public int diffuseAlbedoTex() { return diffuseAlbedoTex; }
    public int specularAlbedoTex() { return specularAlbedoTex; }
    public int normalsTex() { return normalsTex; }
    public int roughnessTex() { return roughnessTex; }
    public int guideDiffuseFmt() { return guideDiffuseFmt; }
    public int guideSpecFmt() { return guideSpecFmt; }
    public int guideNormalsFmt() { return guideNormalsFmt; }
    public int guideRoughFmt() { return guideRoughFmt; }
    public int preDepthTex() { return preDepthTex; }
    public int preMvecTex() { return preMvecTex; }
    public int rawHdrBackupTex() { return rawHdrBackupTex; }
    public int worldDepthSnapshotTex() { return worldDepthSnapshotTex; }
    public MainTarget scaledWorldTarget() { return scaledWorldTarget; }
}
