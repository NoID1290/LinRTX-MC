package dev.linrtx.mc.natives;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * Extracts bundled native Linux libraries (liblinrtx.so + NVIDIA NGX DLSS/DLSS-G)
 * into a persistent runtime directory so ld-linux / NGX Core can resolve them cleanly,
 * and discovers user/mod-local optional NVIDIA TrueHDR directories.
 */
public final class NativeExtractor {
    private static final Logger LOGGER = LoggerFactory.getLogger("LinRTX");
    private static final String BUNDLE_BASE = "/natives/linux-x86_64/";
    private static final String VERSION_TAG = "linrtx-310.9.16-v7";

    private static final List<String> BUNDLED_LIBS = List.of(
        "liblinrtx.so",
        "libnvidia-ngx-dlss.so.310.9.16",
        "libnvidia-ngx-dlssg.so.310.9.16",
        "libnvidia-ngx-dlssd.so.310.9.16"
    );

    public record ExtractedPaths(
        Path libDir,
        Path linrtxSoPath,
        Path logDir,
        String trueHdrSearchDirs,
        String neuralSearchDirs
    ) {}

    private NativeExtractor() {}

    public static ExtractedPaths extractAndDiscover(Path gameDir, String customTrueHdrDir) throws IOException {
        return extractAndDiscover(gameDir, customTrueHdrDir, null);
    }

    public static ExtractedPaths extractAndDiscover(
        Path gameDir,
        String customTrueHdrDir,
        String customNeuralDir
    ) throws IOException {
        Path ngxDir = gameDir.resolve("linrtx-ngx").toAbsolutePath().normalize();
        Path logDir = ngxDir.resolve("logs");
        Files.createDirectories(ngxDir);
        Files.createDirectories(logDir);

        Path stampFile = ngxDir.resolve(".linrtx-version");
        boolean needsExtract = !Files.isRegularFile(stampFile) ||
            !VERSION_TAG.equals(Files.readString(stampFile).trim());

        for (String name : BUNDLED_LIBS) {
            Path target = ngxDir.resolve(name);
            String resPath = BUNDLE_BASE + name;
            boolean hashMatches = !needsExtract && Files.isRegularFile(target) && matchesBundledHash(target, resPath);
            if (!hashMatches) {
                try (InputStream in = NativeExtractor.class.getResourceAsStream(resPath)) {
                    if (in == null) {
                        if (name.startsWith("libnvidia-ngx-")) {
                            if (!Files.isRegularFile(target)) {
                                LOGGER.warn("NVIDIA NGX runtime {} not bundled in JAR and not found at {}; place NGX .so files in {}",
                                    name, target, ngxDir);
                            }
                            continue;
                        }
                        throw new IOException("Missing bundled native library resource: " + resPath);
                    }
                    Path tmp = ngxDir.resolve(name + ".tmp");
                    Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
                    Files.move(tmp, target, StandardCopyOption.REPLACE_EXISTING);
                }
            }
        }

        // Provide unversioned & major-version symlinks expected by NVIDIA NGX loader
        ensureSymlink(ngxDir, "libnvidia-ngx-dlss.so", "libnvidia-ngx-dlss.so.310.9.16");
        ensureSymlink(ngxDir, "libnvidia-ngx-dlss.so.310", "libnvidia-ngx-dlss.so.310.9.16");
        ensureSymlink(ngxDir, "libnvidia-ngx-dlssg.so", "libnvidia-ngx-dlssg.so.310.9.16");
        ensureSymlink(ngxDir, "libnvidia-ngx-dlssg.so.310", "libnvidia-ngx-dlssg.so.310.9.16");
        ensureSymlink(ngxDir, "libnvidia-ngx-dlssd.so", "libnvidia-ngx-dlssd.so.310.9.16");
        ensureSymlink(ngxDir, "libnvidia-ngx-dlssd.so.310", "libnvidia-ngx-dlssd.so.310.9.16");

        Files.writeString(stampFile, VERSION_TAG);

        String hdrSearch = discoverSearchDirs(ngxDir, customTrueHdrDir);
        String neuralSearch = discoverSearchDirs(ngxDir, customNeuralDir);
        LOGGER.info("LinRTX native staging dir: {} (TrueHDR search: {}, DLSS5-NR search: {})",
            ngxDir, hdrSearch, neuralSearch);
        return new ExtractedPaths(ngxDir, ngxDir.resolve("liblinrtx.so"), logDir, hdrSearch, neuralSearch);
    }

    private static void ensureSymlink(Path dir, String linkName, String targetName) {
        Path linkPath = dir.resolve(linkName);
        try {
            if (Files.isSymbolicLink(linkPath)) {
                if (targetName.equals(Files.readSymbolicLink(linkPath).toString())) {
                    return;
                }
                Files.deleteIfExists(linkPath);
            } else if (Files.exists(linkPath)) {
                Files.deleteIfExists(linkPath);
            }
            Files.createSymbolicLink(linkPath, Path.of(targetName));
        } catch (Exception e) {
            LOGGER.debug("Could not create symlink {} -> {}: {}", linkName, targetName, e.getMessage());
        }
    }

    private static boolean matchesBundledHash(Path targetFile, String resourcePath) {
        try (InputStream resIn = NativeExtractor.class.getResourceAsStream(resourcePath);
             InputStream diskIn = Files.newInputStream(targetFile)) {
            if (resIn == null) return false;
            return Arrays.equals(sha256(resIn), sha256(diskIn));
        } catch (IOException e) {
            return false;
        }
    }

    private static byte[] sha256(InputStream in) throws IOException {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                md.update(buf, 0, n);
            }
            return md.digest();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }

    public static String discoverTrueHdrDirs(Path ngxDir, String customDir) {
        return discoverSearchDirs(ngxDir, customDir);
    }

    public static String discoverSearchDirs(Path ngxDir, String customDir) {
        Set<String> dirs = new LinkedHashSet<>();
        if (customDir != null && !customDir.isBlank()) {
            dirs.add(customDir.trim());
        }
        dirs.add(ngxDir.toString());
        return String.join(":", dirs);
    }
}
