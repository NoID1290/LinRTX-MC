package dev.linrtx.mc.render;

import com.mojang.blaze3d.opengl.GlStateManager;
import dev.linrtx.mc.compat.DistantHorizonsCompat.DhDepthSnapshot;
import org.joml.Matrix4fc;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL12;
import org.lwjgl.opengl.GL13;
import org.lwjgl.opengl.GL14;
import org.lwjgl.opengl.GL20;
import org.lwjgl.opengl.GL30;
import org.lwjgl.opengl.GL33;
import org.lwjgl.opengl.GL42;
import org.lwjgl.opengl.GL43;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/**
 * Isolated OpenGL fullscreen-triangle render passes with complete GL + {@link GlStateManager}
 * state save/restore:
 * <ul>
 *   <li>Pass A: Y-flips world color to Vulkan top-left, merges MC & optional Distant Horizons depth
 *       into R32F, and computes normalized backward UV motion vectors (RG16F).</li>
 *   <li>Pass B: Y-flip or straight texture copy between GL textures or to window backbuffer FBO 0.</li>
 * </ul>
 */
public final class GlQuadPass {
    private static final String VERT_SRC = """
        #version 330 core
        out vec2 vUv;
        void main() {
            float x = float((gl_VertexID & 1) << 2) - 1.0;
            float y = float((gl_VertexID & 2) << 1) - 1.0;
            vUv = vec2(x, y) * 0.5 + 0.5;
            gl_Position = vec4(x, y, 0.0, 1.0);
        }
        """;

    private static final String FRAG_MV_SRC = """
        #version 330 core
        in vec2 vUv;
        layout(location = 0) out float outDepth;
        layout(location = 1) out vec2 outMvec;
        layout(location = 2) out vec4 outColor;

        uniform sampler2D uMcColorTex;
        uniform sampler2D uMcDepthTex;
        uniform sampler2D uDhDepthTex;

        uniform mat4 uGlClipCurrToPrevClip;
        uniform mat4 uGlSkyCurrToPrevClip;
        uniform mat4 uMcInvProj;
        uniform mat4 uViewToPrevClip;

        uniform vec2 uJitterNdc;
        uniform float uMcNear;
        uniform float uMcFar;
        uniform int uMcReverseZ;
        uniform int uMcZeroToOne;
        uniform int uFlipOutputY;

        uniform int uUseDhDepth;
        uniform float uDhNear;
        uniform float uDhFar;
        uniform int uDhReverseZ;
        uniform int uDhNegOneToOne;

        float linearizeDhDepth(float dRaw) {
            float z01 = (uDhNegOneToOne != 0) ? (dRaw * 0.5 + 0.5) : dRaw;
            z01 = clamp(z01, 0.0, 1.0);
            float n = max(uDhNear, 0.01);
            float f = max(uDhFar, n + 1.0);
            if (uDhReverseZ != 0) {
                return (n * f) / max(1e-6, z01 * (f - n) + n);
            }
            return (n * f) / max(1e-6, f - z01 * (f - n));
        }

        void main() {
            // uFlipOutputY != 0 outputs Vulkan top-left orientation (row 0 = top) by sampling flipped GL UV;
            // uFlipOutputY == 0 keeps GL bottom-up orientation (for Iris pre-tonemap passes).
            vec2 sampleUv = (uFlipOutputY != 0) ? vec2(vUv.x, 1.0 - vUv.y) : vUv;
            outColor = (uFlipOutputY != 0) ? texture(uMcColorTex, sampleUv) : vec4(0.0);

            float mcDepth = texture(uMcDepthTex, sampleUv).r;
            bool mcIsSky = (uMcReverseZ != 0) ? (mcDepth <= 1e-6) : (mcDepth >= 1.0 - 1e-6);

            vec2 ndcJittered = sampleUv * 2.0 - 1.0;
            // Remove current-frame Halton projection jitter so motion vectors remain unjittered (MVJittered = 0)
            vec2 ndcUnj = ndcJittered - uJitterNdc;
            vec2 uvGlUnj = ndcUnj * 0.5 + 0.5;
            vec2 currTargetUvUnj = (uFlipOutputY != 0) ? vec2(uvGlUnj.x, 1.0 - uvGlUnj.y) : uvGlUnj;

            float zNdc = (uMcReverseZ != 0 || uMcZeroToOne != 0) ? mcDepth : (mcDepth * 2.0 - 1.0);
            vec4 clipPrev = vec4(ndcUnj, 0.0, 1.0);
            float finalDepth = mcDepth;

            if (!mcIsSky) {
                clipPrev = uGlClipCurrToPrevClip * vec4(ndcUnj, zNdc, 1.0);
            } else if (uUseDhDepth != 0) {
                float dhRaw = texture(uDhDepthTex, sampleUv).r;
                bool dhHit = (uDhReverseZ != 0) ? (dhRaw > 1e-6) : (dhRaw < 1.0 - 1e-6);
                if (dhHit) {
                    float dhDist = linearizeDhDepth(dhRaw);
                    vec4 vRay = uMcInvProj * vec4(ndcUnj, 0.5, 1.0);
                    vec3 rayPos = vRay.xyz / (abs(vRay.w) > 1e-6 ? vRay.w : 1.0);
                    vec3 viewPos = rayPos * (dhDist / max(1e-4, -rayPos.z));
                    clipPrev = uViewToPrevClip * vec4(viewPos, 1.0);
                    float n = max(uMcNear, 0.01);
                    float dhRev = clamp(n / max(dhDist, n), 1e-6, 1.0);
                    finalDepth = (uMcReverseZ != 0) ? dhRev : (1.0 - dhRev);
                } else {
                    float skyZ = (uMcReverseZ != 0) ? 1e-4 : (uMcZeroToOne != 0 ? 0.9999 : 0.9998);
                    clipPrev = uGlSkyCurrToPrevClip * vec4(ndcUnj, skyZ, 1.0);
                }
            } else {
                float skyZ = (uMcReverseZ != 0) ? 1e-4 : (uMcZeroToOne != 0 ? 0.9999 : 0.9998);
                clipPrev = uGlSkyCurrToPrevClip * vec4(ndcUnj, skyZ, 1.0);
            }

            vec2 prevNdc = clipPrev.xy / (abs(clipPrev.w) > 1e-6 ? clipPrev.w : 1.0);
            vec2 prevUvGl = prevNdc * 0.5 + 0.5;
            vec2 prevTargetUv = (uFlipOutputY != 0) ? vec2(prevUvGl.x, 1.0 - prevUvGl.y) : prevUvGl;

            // Normalized backward motion vector: prevUV - currUV
            vec2 mv = (clipPrev.w > 1e-5) ? (prevTargetUv - currTargetUvUnj) : vec2(0.0);
            outMvec = mv;
            outDepth = finalDepth;
        }
        """;

    private static final String FRAG_COPY_SRC = """
        #version 330 core
        in vec2 vUv;
        out vec4 outColor;
        uniform sampler2D uSrcTex;
        uniform int uFlipY;
        void main() {
            vec2 uv = (uFlipY != 0) ? vec2(vUv.x, 1.0 - vUv.y) : vUv;
            outColor = texture(uSrcTex, uv);
        }
        """;

    private static final int[] DRAW_BUFFERS_3 = {
        GL30.GL_COLOR_ATTACHMENT0, GL30.GL_COLOR_ATTACHMENT1, GL30.GL_COLOR_ATTACHMENT2
    };
    private static final int[] DRAW_BUFFERS_2 = {
        GL30.GL_COLOR_ATTACHMENT0, GL30.GL_COLOR_ATTACHMENT1
    };
    private static final int[] DRAW_BUFFERS_1 = {GL30.GL_COLOR_ATTACHMENT0};

    private int emptyVao = 0;
    private int fbo = 0;
    private int mvProgram = 0;
    private int copyProgram = 0;
    private int linearSampler = 0;
    private int nearestSampler = 0;

    // Cached MV uniform locations
    private int uMcColorTex, uMcDepthTex, uDhDepthTex;
    private int uGlClipCurrToPrevClip, uGlSkyCurrToPrevClip, uMcInvProj, uViewToPrevClip;
    private int uJitterNdc, uMcNear, uMcFar, uMcReverseZ, uMcZeroToOne, uFlipOutputY;
    private int uUseDhDepth, uDhNear, uDhFar, uDhReverseZ, uDhNegOneToOne;

    // Cached Copy uniform locations
    private int uCopySrcTex, uCopyFlipY;

    private final FloatBuffer matBuf = ByteBuffer.allocateDirect(16 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
    private final ByteBuffer colorMaskBuf = ByteBuffer.allocateDirect(4);
    private final int[] prevViewport = new int[4];

    private void ensureInitialized() {
        if (emptyVao != 0) {
            return;
        }
        emptyVao = GL30.glGenVertexArrays();
        fbo = GL30.glGenFramebuffers();
        linearSampler = GL33.glGenSamplers();
        GL33.glSamplerParameteri(linearSampler, GL11.GL_TEXTURE_MIN_FILTER, GL11.GL_LINEAR);
        GL33.glSamplerParameteri(linearSampler, GL11.GL_TEXTURE_MAG_FILTER, GL11.GL_LINEAR);
        GL33.glSamplerParameteri(linearSampler, GL11.GL_TEXTURE_WRAP_S, GL12.GL_CLAMP_TO_EDGE);
        GL33.glSamplerParameteri(linearSampler, GL11.GL_TEXTURE_WRAP_T, GL12.GL_CLAMP_TO_EDGE);
        GL33.glSamplerParameteri(linearSampler, GL14.GL_TEXTURE_COMPARE_MODE, GL11.GL_NONE);

        nearestSampler = GL33.glGenSamplers();
        GL33.glSamplerParameteri(nearestSampler, GL11.GL_TEXTURE_MIN_FILTER, GL11.GL_NEAREST);
        GL33.glSamplerParameteri(nearestSampler, GL11.GL_TEXTURE_MAG_FILTER, GL11.GL_NEAREST);
        GL33.glSamplerParameteri(nearestSampler, GL11.GL_TEXTURE_WRAP_S, GL12.GL_CLAMP_TO_EDGE);
        GL33.glSamplerParameteri(nearestSampler, GL11.GL_TEXTURE_WRAP_T, GL12.GL_CLAMP_TO_EDGE);
        GL33.glSamplerParameteri(nearestSampler, GL14.GL_TEXTURE_COMPARE_MODE, GL11.GL_NONE);

        int vs = compileShader(GL20.GL_VERTEX_SHADER, VERT_SRC);
        int fsMv = compileShader(GL20.GL_FRAGMENT_SHADER, FRAG_MV_SRC);
        mvProgram = linkProgram(vs, fsMv);
        GL20.glDeleteShader(fsMv);

        int fsCopy = compileShader(GL20.GL_FRAGMENT_SHADER, FRAG_COPY_SRC);
        copyProgram = linkProgram(vs, fsCopy);
        GL20.glDeleteShader(fsCopy);
        GL20.glDeleteShader(vs);

        uMcColorTex = GL20.glGetUniformLocation(mvProgram, "uMcColorTex");
        uMcDepthTex = GL20.glGetUniformLocation(mvProgram, "uMcDepthTex");
        uDhDepthTex = GL20.glGetUniformLocation(mvProgram, "uDhDepthTex");
        uGlClipCurrToPrevClip = GL20.glGetUniformLocation(mvProgram, "uGlClipCurrToPrevClip");
        uGlSkyCurrToPrevClip = GL20.glGetUniformLocation(mvProgram, "uGlSkyCurrToPrevClip");
        uMcInvProj = GL20.glGetUniformLocation(mvProgram, "uMcInvProj");
        uViewToPrevClip = GL20.glGetUniformLocation(mvProgram, "uViewToPrevClip");
        uJitterNdc = GL20.glGetUniformLocation(mvProgram, "uJitterNdc");
        uMcNear = GL20.glGetUniformLocation(mvProgram, "uMcNear");
        uMcFar = GL20.glGetUniformLocation(mvProgram, "uMcFar");
        uMcReverseZ = GL20.glGetUniformLocation(mvProgram, "uMcReverseZ");
        uMcZeroToOne = GL20.glGetUniformLocation(mvProgram, "uMcZeroToOne");
        uFlipOutputY = GL20.glGetUniformLocation(mvProgram, "uFlipOutputY");
        uUseDhDepth = GL20.glGetUniformLocation(mvProgram, "uUseDhDepth");
        uDhNear = GL20.glGetUniformLocation(mvProgram, "uDhNear");
        uDhFar = GL20.glGetUniformLocation(mvProgram, "uDhFar");
        uDhReverseZ = GL20.glGetUniformLocation(mvProgram, "uDhReverseZ");
        uDhNegOneToOne = GL20.glGetUniformLocation(mvProgram, "uDhNegOneToOne");

        uCopySrcTex = GL20.glGetUniformLocation(copyProgram, "uSrcTex");
        uCopyFlipY = GL20.glGetUniformLocation(copyProgram, "uFlipY");
    }

    public void runMotionVectorPass(
        int srcMcColorTex,
        int srcMcDepthTex,
        boolean mcReverseZ,
        boolean mcZeroToOne,
        float jitterX,
        float jitterY,
        DhDepthSnapshot dh,
        boolean useDh,
        CameraStateTracker cam,
        int dstColorInTex,
        int dstDepthInTex,
        int dstMvecInTex,
        int width,
        int height
    ) {
        runMvInternal(srcMcColorTex, srcMcDepthTex, mcReverseZ, mcZeroToOne, jitterX, jitterY, dh, useDh, cam,
            dstColorInTex, dstDepthInTex, dstMvecInTex, width, height, true);
    }

    /**
     * Generates bottom-up GL R32F depth and RG16F motion vectors (no Y-flip) for
     * Iris pre-tonemap linear HDR denoising (e.g. Camellia colortex5).
     */
    public void runPreTonemapDepthMvPass(
        int srcMcDepthTex,
        boolean mcReverseZ,
        boolean mcZeroToOne,
        float jitterX,
        float jitterY,
        DhDepthSnapshot dh,
        boolean useDh,
        CameraStateTracker cam,
        int dstDepthTex,
        int dstMvecTex,
        int width,
        int height
    ) {
        runMvInternal(srcMcDepthTex, srcMcDepthTex, mcReverseZ, mcZeroToOne, jitterX, jitterY, dh, useDh, cam,
            0, dstDepthTex, dstMvecTex, width, height, false);
    }

    private void runMvInternal(
        int srcMcColorTex,
        int srcMcDepthTex,
        boolean mcReverseZ,
        boolean mcZeroToOne,
        float jitterX,
        float jitterY,
        DhDepthSnapshot dh,
        boolean useDh,
        CameraStateTracker cam,
        int dstColorInTex,
        int dstDepthInTex,
        int dstMvecInTex,
        int width,
        int height,
        boolean flipOutputY
    ) {
        ensureInitialized();
        runWithSavedState(width, height, () -> {
            GL30.glBindFramebuffer(GL30.GL_FRAMEBUFFER, fbo);
            GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT0, GL11.GL_TEXTURE_2D, dstDepthInTex, 0);
            GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT1, GL11.GL_TEXTURE_2D, dstMvecInTex, 0);
            if (flipOutputY && dstColorInTex != 0) {
                GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT2, GL11.GL_TEXTURE_2D, dstColorInTex, 0);
                GL20.glDrawBuffers(DRAW_BUFFERS_3);
            } else {
                GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT2, GL11.GL_TEXTURE_2D, 0, 0);
                GL20.glDrawBuffers(DRAW_BUFFERS_2);
            }

            GL20.glUseProgram(mvProgram);
            bindTexUnit(0, srcMcColorTex != 0 ? srcMcColorTex : srcMcDepthTex);
            GL33.glBindSampler(0, nearestSampler);
            bindTexUnit(1, srcMcDepthTex);
            GL33.glBindSampler(1, nearestSampler);
            bindTexUnit(2, (useDh && dh != null && dh.valid && dh.depthGlId > 0) ? dh.depthGlId : srcMcDepthTex);
            GL33.glBindSampler(2, nearestSampler);

            GL20.glUniform1i(uMcColorTex, 0);
            GL20.glUniform1i(uMcDepthTex, 1);
            GL20.glUniform1i(uDhDepthTex, 2);

            uploadMat4(uGlClipCurrToPrevClip, cam.glClipCurrToPrevClip());
            uploadMat4(uGlSkyCurrToPrevClip, cam.glSkyCurrToPrevClip());
            uploadMat4(uMcInvProj, cam.mcInvProjGl());
            uploadMat4(uViewToPrevClip, cam.glViewToPrevClip());

            float jNdcX = HaltonJitter.ndcOffsetX(jitterX, width);
            float jNdcY = HaltonJitter.ndcOffsetY(jitterY, height);
            GL20.glUniform2f(uJitterNdc, jNdcX, jNdcY);
            GL20.glUniform1f(uMcNear, cam.cameraNear());
            GL20.glUniform1f(uMcFar, cam.cameraFar());
            GL20.glUniform1i(uMcReverseZ, mcReverseZ ? 1 : 0);
            GL20.glUniform1i(uMcZeroToOne, mcZeroToOne ? 1 : 0);
            GL20.glUniform1i(uFlipOutputY, flipOutputY ? 1 : 0);

            boolean activeDh = useDh && dh != null && dh.valid && dh.depthGlId > 0;
            GL20.glUniform1i(uUseDhDepth, activeDh ? 1 : 0);
            GL20.glUniform1f(uDhNear, activeDh ? dh.nearClip : 16.0f);
            GL20.glUniform1f(uDhFar, activeDh ? dh.farClip : 16384.0f);
            GL20.glUniform1i(uDhReverseZ, (activeDh && dh.reverseZ) ? 1 : 0);
            GL20.glUniform1i(uDhNegOneToOne, (activeDh && dh.negOneToPosOne) ? 1 : 0);

            GL11.glDrawArrays(GL11.GL_TRIANGLES, 0, 3);

            GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT0, GL11.GL_TEXTURE_2D, 0, 0);
            GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT1, GL11.GL_TEXTURE_2D, 0, 0);
            GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT2, GL11.GL_TEXTURE_2D, 0, 0);
        });
    }

    /**
     * Exact GPU-side image copy preserving bit-identical pixel formats (e.g. RGBA32F backup/restore).
     */
    public void copyImageDirect(int srcTex, int dstTex, int width, int height) {
        GL43.glCopyImageSubData(
            srcTex, GL11.GL_TEXTURE_2D, 0, 0, 0, 0,
            dstTex, GL11.GL_TEXTURE_2D, 0, 0, 0, 0,
            width, height, 1
        );
        GL42.glMemoryBarrier(
            GL42.GL_TEXTURE_FETCH_BARRIER_BIT |
            GL42.GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
            GL42.GL_FRAMEBUFFER_BARRIER_BIT
        );
    }

    public void memoryBarrierAll() {
        GL42.glMemoryBarrier(GL42.GL_ALL_BARRIER_BITS);
    }

    public void copyToTexture(int srcTex, int dstTex, int width, int height, boolean flipY) {
        copyToTexture(srcTex, dstTex, width, height, flipY, false);
    }

    public void copyToTexture(int srcTex, int dstTex, int width, int height, boolean flipY, boolean linearFilter) {
        ensureInitialized();
        runWithSavedState(width, height, () -> {
            GL30.glBindFramebuffer(GL30.GL_FRAMEBUFFER, fbo);
            GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT0, GL11.GL_TEXTURE_2D, dstTex, 0);
            GL20.glDrawBuffers(DRAW_BUFFERS_1);

            GL20.glUseProgram(copyProgram);
            bindTexUnit(0, srcTex);
            GL33.glBindSampler(0, linearFilter ? linearSampler : nearestSampler);
            GL20.glUniform1i(uCopySrcTex, 0);
            GL20.glUniform1i(uCopyFlipY, flipY ? 1 : 0);
            GL11.glDrawArrays(GL11.GL_TRIANGLES, 0, 3);

            GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT0, GL11.GL_TEXTURE_2D, 0, 0);
        });
    }

    public void presentToDefaultFramebuffer(int srcTex, int width, int height, boolean flipY) {
        ensureInitialized();
        runWithSavedState(width, height, () -> {
            GL30.glBindFramebuffer(GL30.GL_FRAMEBUFFER, 0);
            GL11.glDrawBuffer(GL11.GL_BACK);

            GL20.glUseProgram(copyProgram);
            bindTexUnit(0, srcTex);
            GL20.glUniform1i(uCopySrcTex, 0);
            GL20.glUniform1i(uCopyFlipY, flipY ? 1 : 0);
            GL11.glDrawArrays(GL11.GL_TRIANGLES, 0, 3);
        });
    }

    public void destroy() {
        if (linearSampler != 0) {
            GL33.glDeleteSamplers(linearSampler);
            linearSampler = 0;
        }
        if (nearestSampler != 0) {
            GL33.glDeleteSamplers(nearestSampler);
            nearestSampler = 0;
        }
        if (mvProgram != 0) {
            GL20.glDeleteProgram(mvProgram);
            mvProgram = 0;
        }
        if (copyProgram != 0) {
            GL20.glDeleteProgram(copyProgram);
            copyProgram = 0;
        }
        if (fbo != 0) {
            GL30.glDeleteFramebuffers(fbo);
            fbo = 0;
        }
        if (emptyVao != 0) {
            GL30.glDeleteVertexArrays(emptyVao);
            emptyVao = 0;
        }
    }

    private void uploadMat4(int loc, Matrix4fc m) {
        matBuf.clear();
        m.get(matBuf);
        matBuf.rewind();
        GL20.glUniformMatrix4fv(loc, false, matBuf);
    }

    private static void bindTexUnit(int unit, int texId) {
        GL13.glActiveTexture(GL13.GL_TEXTURE0 + unit);
        GL11.glBindTexture(GL11.GL_TEXTURE_2D, texId);
    }

    private void runWithSavedState(int width, int height, Runnable body) {
        int prevProg = GL11.glGetInteger(GL20.GL_CURRENT_PROGRAM);
        int prevVao = GL11.glGetInteger(GL30.GL_VERTEX_ARRAY_BINDING);
        int prevDrawFbo = GL11.glGetInteger(GL30.GL_DRAW_FRAMEBUFFER_BINDING);
        int prevReadFbo = GL11.glGetInteger(GL30.GL_READ_FRAMEBUFFER_BINDING);
        int prevActiveTex = GL11.glGetInteger(GL13.GL_ACTIVE_TEXTURE);
        GL11.glGetIntegerv(GL11.GL_VIEWPORT, prevViewport);

        GL13.glActiveTexture(GL13.GL_TEXTURE0);
        int prevTex0 = GL11.glGetInteger(GL11.GL_TEXTURE_BINDING_2D);
        int prevSampler0 = GL11.glGetInteger(GL33.GL_SAMPLER_BINDING);
        GL13.glActiveTexture(GL13.GL_TEXTURE1);
        int prevTex1 = GL11.glGetInteger(GL11.GL_TEXTURE_BINDING_2D);
        int prevSampler1 = GL11.glGetInteger(GL33.GL_SAMPLER_BINDING);
        GL13.glActiveTexture(GL13.GL_TEXTURE2);
        int prevTex2 = GL11.glGetInteger(GL11.GL_TEXTURE_BINDING_2D);
        int prevSampler2 = GL11.glGetInteger(GL33.GL_SAMPLER_BINDING);

        colorMaskBuf.clear();
        GL11.glGetBooleanv(GL11.GL_COLOR_WRITEMASK, colorMaskBuf);
        boolean prevMaskR = colorMaskBuf.get(0) != 0;
        boolean prevMaskG = colorMaskBuf.get(1) != 0;
        boolean prevMaskB = colorMaskBuf.get(2) != 0;
        boolean prevMaskA = colorMaskBuf.get(3) != 0;
        boolean prevDepthMask = GL11.glGetBoolean(GL11.GL_DEPTH_WRITEMASK);

        boolean hadDepth = GL11.glIsEnabled(GL11.GL_DEPTH_TEST);
        boolean hadScissor = GL11.glIsEnabled(GL11.GL_SCISSOR_TEST);
        boolean hadBlend = GL11.glIsEnabled(GL11.GL_BLEND);
        boolean hadCull = GL11.glIsEnabled(GL11.GL_CULL_FACE);
        boolean hadSrgb = GL11.glIsEnabled(GL30.GL_FRAMEBUFFER_SRGB);

        GL11.glDisable(GL11.GL_DEPTH_TEST);
        GL11.glDisable(GL11.GL_SCISSOR_TEST);
        GL11.glDisable(GL11.GL_BLEND);
        GL11.glDisable(GL11.GL_CULL_FACE);
        GL11.glDisable(GL30.GL_FRAMEBUFFER_SRGB);
        GL11.glColorMask(true, true, true, true);
        GL11.glDepthMask(false);
        GL33.glBindSampler(0, 0);
        GL33.glBindSampler(1, 0);
        GL33.glBindSampler(2, 0);
        GL11.glViewport(0, 0, width, height);
        GL30.glBindVertexArray(emptyVao);

        try {
            body.run();
        } finally {
            // Restore raw GL directly (never via GlStateManager) so Iris & Sodium state caches
            // cannot drop or mis-track viewport, program, or FBO restores.
            GL30.glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, prevReadFbo);
            GL30.glBindFramebuffer(GL30.GL_DRAW_FRAMEBUFFER, prevDrawFbo);
            GL30.glBindVertexArray(prevVao);
            GL20.glUseProgram(prevProg);

            bindTexUnit(0, prevTex0);
            GL33.glBindSampler(0, prevSampler0);
            bindTexUnit(1, prevTex1);
            GL33.glBindSampler(1, prevSampler1);
            bindTexUnit(2, prevTex2);
            GL33.glBindSampler(2, prevSampler2);
            GL13.glActiveTexture(prevActiveTex);

            if (hadDepth) GL11.glEnable(GL11.GL_DEPTH_TEST);
            else GL11.glDisable(GL11.GL_DEPTH_TEST);
            if (hadScissor) GL11.glEnable(GL11.GL_SCISSOR_TEST);
            else GL11.glDisable(GL11.GL_SCISSOR_TEST);
            if (hadBlend) GL11.glEnable(GL11.GL_BLEND);
            else GL11.glDisable(GL11.GL_BLEND);
            if (hadCull) GL11.glEnable(GL11.GL_CULL_FACE);
            else GL11.glDisable(GL11.GL_CULL_FACE);
            if (hadSrgb) GL11.glEnable(GL30.GL_FRAMEBUFFER_SRGB);
            else GL11.glDisable(GL30.GL_FRAMEBUFFER_SRGB);

            GL11.glColorMask(prevMaskR, prevMaskG, prevMaskB, prevMaskA);
            GL11.glDepthMask(prevDepthMask);
            GL11.glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        }
    }

    private static int compileShader(int type, String src) {
        int shader = GL20.glCreateShader(type);
        GL20.glShaderSource(shader, src);
        GL20.glCompileShader(shader);
        if (GL20.glGetShaderi(shader, GL20.GL_COMPILE_STATUS) == GL11.GL_FALSE) {
            String log = GL20.glGetShaderInfoLog(shader);
            GL20.glDeleteShader(shader);
            throw new IllegalStateException("LinRTX GLSL compile error: " + log);
        }
        return shader;
    }

    private static int linkProgram(int vs, int fs) {
        int prog = GL20.glCreateProgram();
        GL20.glAttachShader(prog, vs);
        GL20.glAttachShader(prog, fs);
        GL20.glLinkProgram(prog);
        if (GL20.glGetProgrami(prog, GL20.GL_LINK_STATUS) == GL11.GL_FALSE) {
            String log = GL20.glGetProgramInfoLog(prog);
            GL20.glDeleteProgram(prog);
            throw new IllegalStateException("LinRTX GLSL link error: " + log);
        }
        return prog;
    }
}
