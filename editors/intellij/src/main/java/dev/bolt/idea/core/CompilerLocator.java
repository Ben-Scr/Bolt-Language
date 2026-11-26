package dev.bolt.idea.core;

import java.io.*;
import java.nio.file.*;
import java.util.*;

/** IDEs launched from the macOS Dock need not inherit a user's interactive-shell PATH. */
public final class CompilerLocator {
    private CompilerLocator() { }
    public static Path locate(String configured) throws IOException {
        String name = System.getProperty("os.name", "").toLowerCase(Locale.ROOT).contains("win") ? "bolt.exe" : "bolt";
        List<Path> candidates = new ArrayList<>();
        String chosen = configured == null ? "" : configured.trim();
        if (!chosen.isEmpty()) {
            if (chosen.startsWith("~/")) chosen = System.getProperty("user.home") + chosen.substring(1);
            Path path = Path.of(chosen);
            if (path.isAbsolute() || chosen.contains("/") || chosen.contains("\\")) {
                if (!path.isAbsolute()) throw new IOException("Use an absolute Bolt executable path, not a project-relative path.");
                return requireExecutable(path);
            }
            name = chosen;
            if (System.getProperty("os.name", "").startsWith("Windows") && !name.toLowerCase(Locale.ROOT).endsWith(".exe")) name += ".exe";
        }
        String override = System.getenv("BOLT_COMPILER");
        if (chosen.isEmpty() && override != null && !override.isBlank()) {
            Path overridden = Path.of(override.startsWith("~/") ? System.getProperty("user.home") + override.substring(1) : override);
            if (!overridden.isAbsolute()) throw new IOException("BOLT_COMPILER must be an absolute path.");
            return requireExecutable(overridden);
        }
        for (String dir : System.getenv().getOrDefault("PATH", "").split(java.util.regex.Pattern.quote(File.pathSeparator))) {
            if (!dir.isBlank()) {
                Path folder = Path.of(dir);
                // Do not execute binaries from untrusted project-relative PATH entries.
                if (folder.isAbsolute()) candidates.add(folder.resolve(name));
            }
        }
        Path home = Path.of(System.getProperty("user.home"));
        candidates.add(home.resolve(".local/share/bolt/bin/" + name));
        candidates.add(home.resolve(".local/share/bolt/libexec/" + name));
        candidates.add(home.resolve(".local/bin/" + name));
        if (name.endsWith(".exe")) {
            String local = System.getenv("LOCALAPPDATA");
            if (local != null) {
                candidates.add(Path.of(local, "Programs", "Bolt", "libexec", name));
                candidates.add(Path.of(local, "Programs", "Bolt", "bin", name));
            }
        } else {
            candidates.add(Path.of("/opt/homebrew/bin", name));
            candidates.add(Path.of("/usr/local/bin", name));
        }
        for (Path candidate : candidates) if (Files.isRegularFile(candidate) && Files.isExecutable(candidate)) return requireExecutable(candidate);
        throw new IOException("Bolt executable not found. Set its absolute path in Settings > Tools > Bolt. " +
            "On macOS the default installation is ~/.local/share/bolt/bin/bolt.");
    }
    private static Path requireExecutable(Path path) throws IOException {
        String lower = path.getFileName().toString().toLowerCase(Locale.ROOT);
        if (lower.endsWith(".cmd") || lower.endsWith(".bat")) {
            Path actual = path.toAbsolutePath().normalize().getParent().resolve("../libexec/bolt.exe").normalize();
            if (Files.isRegularFile(actual)) return requireExecutable(actual);
            throw new IOException("Select bolt.exe, not a .cmd/.bat wrapper.");
        }
        if (!Files.isRegularFile(path) || !Files.isExecutable(path)) throw new IOException("Not an executable Bolt file: " + path);
        return path.toRealPath();
    }
}
