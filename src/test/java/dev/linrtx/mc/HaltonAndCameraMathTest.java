package dev.linrtx.mc;

import dev.linrtx.mc.render.CameraStateTracker;
import dev.linrtx.mc.render.HaltonJitter;
import org.joml.Matrix4f;
import org.joml.Vector4f;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class HaltonAndCameraMathTest {
    @Test
    void haltonJitterBoundsAndCycleLength() {
        HaltonJitter jitter = new HaltonJitter();
        assertEquals(16, HaltonJitter.computeCycleLength(1920, 1920));
        assertEquals(72, HaltonJitter.computeCycleLength(1280, 3840));

        for (int i = 0; i < 64; i++) {
            jitter.advance(1708, 2560);
            assertTrue(jitter.currentX() >= -0.5f && jitter.currentX() < 0.5f);
            assertTrue(jitter.currentY() >= -0.5f && jitter.currentY() < 0.5f);
        }
    }

    @Test
    void projectionJitterMatchesNgxVulkanSignAndMotionVectorUnjitter() {
        int renderW = 1280;
        int renderH = 720;
        HaltonJitter jitter = new HaltonJitter();
        Matrix4f baseProj = new Matrix4f().perspective((float) Math.toRadians(70.0), (float) renderW / renderH, 0.05f, 256.0f, true);
        Vector4f viewPt = new Vector4f(0.75f, -0.4f, -4.0f, 1.0f);

        Vector4f baseClip = new Vector4f(viewPt).mul(baseProj);
        float baseNdcX = baseClip.x / baseClip.w;
        float baseNdcY = baseClip.y / baseClip.w;
        float baseVkPxX = (baseNdcX * 0.5f + 0.5f) * renderW;
        float baseVkPxY = (0.5f - baseNdcY * 0.5f) * renderH;

        for (int frame = 0; frame < 16; frame++) {
            jitter.advance(renderW, renderW);
            float jx = jitter.currentX();
            float jy = jitter.currentY();

            Matrix4f jitProj = new Matrix4f(baseProj);
            jitter.applyProjectionJitter(jitProj, renderW, renderH);

            Vector4f jitClip = new Vector4f(viewPt).mul(jitProj);
            float jitNdcX = jitClip.x / jitClip.w;
            float jitNdcY = jitClip.y / jitClip.w;
            float jitVkPxX = (jitNdcX * 0.5f + 0.5f) * renderW;
            float jitVkPxY = (0.5f - jitNdcY * 0.5f) * renderH;

            // NVIDIA NGX DLSS-SR / DLSS-RR expects InJitterOffset = (+jx, +jy) to match
            // geometry displacement (+jx, +jy) in Vulkan top-left pixel coordinates.
            assertEquals(jx, jitVkPxX - baseVkPxX, 1e-3f, "Vulkan X geometry shift must equal +jx");
            assertEquals(jy, jitVkPxY - baseVkPxY, 1e-3f, "Vulkan Y geometry shift must equal +jy");

            // GlQuadPass subtracts uJitterNdc to recover unjittered NDC (zero static-camera MV).
            float unjNdcX = jitNdcX - HaltonJitter.ndcOffsetX(jx, renderW);
            float unjNdcY = jitNdcY - HaltonJitter.ndcOffsetY(jy, renderH);
            assertEquals(baseNdcX, unjNdcX, 1e-5f, "Motion-vector unjitter must recover base NDC X");
            assertEquals(baseNdcY, unjNdcY, 1e-5f, "Motion-vector unjitter must recover base NDC Y");
        }
    }

    @Test
    void staticCameraProducesIdentityMotionMatrices() {
        CameraStateTracker tracker = new CameraStateTracker();
        // MC Reverse-Z perspective matrix: far -> 0, near -> 1 in [0, 1] NDC
        Matrix4f proj = new Matrix4f().perspective((float) Math.toRadians(70.0), 16.0f / 9.0f, 0.05f, 256.0f, true);
        // Flip Z into Reverse-Z [0..1] simply by inverting z mapping on perspective; identity test holds for any invertible P
        Matrix4f viewRot = new Matrix4f().identity();

        tracker.update("world1", 100.5, 64.0, -250.25, proj, viewRot, 0.05f, 256.0f, (float) Math.toRadians(70.0), 16.0f / 9.0f);
        assertTrue(tracker.checkAndClearReset());

        tracker.update("world1", 100.5, 64.0, -250.25, proj, viewRot, 0.05f, 256.0f, (float) Math.toRadians(70.0), 16.0f / 9.0f);
        assertFalse(tracker.checkAndClearReset());

        Vector4f testClip = new Vector4f(0.25f, -0.35f, 0.4f, 1.0f);
        Vector4f prev = new Vector4f(testClip).mul(tracker.glClipCurrToPrevClip());
        assertEquals(testClip.x, prev.x, 1e-4f);
        assertEquals(testClip.y, prev.y, 1e-4f);
        assertEquals(testClip.z, prev.z, 1e-4f);
    }

    @Test
    void panningCameraRightYieldsPositiveBackwardMotionVector() {
        CameraStateTracker tracker = new CameraStateTracker();
        Matrix4f proj = new Matrix4f().perspective((float) Math.toRadians(90.0), 1.0f, 0.1f, 100.0f, true);
        Matrix4f rot = new Matrix4f().identity();

        // Point fixed in world at (0, 0, -5). Frame 0 camera at origin -> projects at center u_tl=0.5, v_tl=0.5
        tracker.update("w", 0.0, 0.0, 0.0, proj, rot, 0.1f, 100.0f, (float) Math.toRadians(90.0), 1.0f);
        tracker.checkAndClearReset();

        // Frame 1 camera shifts +1.0 along +X (right). Point in frame 1 camera-rel space is (-1, 0, -5), which moves LEFT on screen.
        tracker.update("w", 1.0, 0.0, 0.0, proj, rot, 0.1f, 100.0f, (float) Math.toRadians(90.0), 1.0f);

        Vector4f pCurr = new Vector4f(-1.0f, 0.0f, -5.0f, 1.0f).mul(proj);
        float ndcXCurr = pCurr.x / pCurr.w;
        float ndcYCurr = -pCurr.y / pCurr.w; // Vulkan top-left Y
        float ndcZCurr = pCurr.z / pCurr.w;

        Matrix4f clipToPrevVk = new Matrix4f().set(tracker.clipToPrevClipVk());
        Vector4f pPrevVk = new Vector4f(ndcXCurr, ndcYCurr, ndcZCurr, 1.0f).mul(clipToPrevVk);
        float ndcXPrev = pPrevVk.x / pPrevVk.w;
        float uvXCurr = ndcXCurr * 0.5f + 0.5f;
        float uvXPrev = ndcXPrev * 0.5f + 0.5f;

        assertEquals(0.5f, uvXPrev, 1e-4f);
        assertTrue((uvXPrev - uvXCurr) > 0.05f, "Backward MV must point right toward previous screen X");
    }

    @Test
    void teleportResetsHistory() {
        CameraStateTracker tracker = new CameraStateTracker();
        Matrix4f proj = new Matrix4f().perspective((float) Math.toRadians(70.0), 1.77f, 0.05f, 256.0f, true);
        Matrix4f rot = new Matrix4f().identity();

        tracker.update("w", 0.0, 64.0, 0.0, proj, rot, 0.05f, 256.0f, 1.2f, 1.77f);
        assertTrue(tracker.checkAndClearReset());

        tracker.update("w", 500.0, 64.0, 0.0, proj, rot, 0.05f, 256.0f, 1.2f, 1.77f);
        assertTrue(tracker.checkAndClearReset());
    }

    @Test
    void skyMotionMatrixIgnoresTranslationButReprojectsRotation() {
        Matrix4f proj = new Matrix4f().perspective((float) Math.toRadians(70.0), 16.0f / 9.0f, 0.05f, 256.0f, false);
        Matrix4f rot0 = new Matrix4f().identity();
        Matrix4f rot1 = new Matrix4f().rotateY((float) Math.toRadians(12.0));

        CameraStateTracker pureTranslate = new CameraStateTracker();
        pureTranslate.update("w", 0.0, 64.0, 0.0, proj, rot0, 0.05f, 256.0f, 1.0f, 1.0f);
        pureTranslate.checkAndClearReset();
        pureTranslate.update("w", 10.0, 72.0, -8.0, proj, rot0, 0.05f, 256.0f, 1.0f, 1.0f);

        Vector4f testClip = new Vector4f(0.25f, -0.35f, 0.9f, 1.0f);
        Vector4f skyPrev = new Vector4f(testClip).mul(pureTranslate.glSkyCurrToPrevClip());
        assertEquals(testClip.x, skyPrev.x / skyPrev.w, 1e-4f);
        assertEquals(testClip.y, skyPrev.y / skyPrev.w, 1e-4f);
        assertEquals(testClip.z, skyPrev.z / skyPrev.w, 1e-4f);

        CameraStateTracker rotWithTranslate = new CameraStateTracker();
        rotWithTranslate.update("w", 0.0, 64.0, 0.0, proj, rot0, 0.05f, 256.0f, 1.0f, 1.0f);
        rotWithTranslate.checkAndClearReset();
        rotWithTranslate.update("w", 12.0, 68.0, -6.0, proj, rot1, 0.05f, 256.0f, 1.0f, 1.0f);

        CameraStateTracker rotNoTranslate = new CameraStateTracker();
        rotNoTranslate.update("w", 0.0, 64.0, 0.0, proj, rot0, 0.05f, 256.0f, 1.0f, 1.0f);
        rotNoTranslate.checkAndClearReset();
        rotNoTranslate.update("w", 0.0, 64.0, 0.0, proj, rot1, 0.05f, 256.0f, 1.0f, 1.0f);

        Vector4f skyA = new Vector4f(testClip).mul(rotWithTranslate.glSkyCurrToPrevClip());
        Vector4f skyB = new Vector4f(testClip).mul(rotNoTranslate.glSkyCurrToPrevClip());
        assertEquals(skyB.x / skyB.w, skyA.x / skyA.w, 1e-4f);
        assertEquals(skyB.y / skyB.w, skyA.y / skyA.w, 1e-4f);
        assertTrue(Math.abs((skyA.x / skyA.w) - testClip.x) > 0.05f, "Yaw rotation must shift sky reprojection");
    }

    @Test
    void cameraFovAndAspectDerivedFromProjection() {
        CameraStateTracker tracker = new CameraStateTracker();
        float expectedFov = (float) Math.toRadians(70.0);
        float expectedAspect = 16.0f / 9.0f;
        Matrix4f proj = new Matrix4f().perspective(expectedFov, expectedAspect, 0.05f, 256.0f, false);

        // Intentionally pass mismatched scalar fallback values to verify projection matrix extraction wins
        tracker.update("w", 0.0, 64.0, 0.0, proj, new Matrix4f().identity(), 0.05f, 256.0f, 0.5f, 1.0f);
        assertEquals(expectedFov, tracker.cameraFovRadians(), 1e-4f);
        assertEquals(expectedAspect, tracker.cameraAspect(), 1e-4f);
    }
}
