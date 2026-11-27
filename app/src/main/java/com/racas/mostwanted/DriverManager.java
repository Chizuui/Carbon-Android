package com.racas.mostwanted;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// Picks and extracts a Turnip / AdrenoTools custom Vulkan driver (.zip or .so) into app-private storage.
final class DriverManager {
    static final int REQUEST = 0x4452;  // "DR"

    interface Callback {
        void done(boolean ok, String dir, String lib, String name, String error);
    }

    static boolean pick(Activity activity) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/octet-stream"});
        try {
            activity.startActivityForResult(intent, REQUEST);
            return true;
        } catch (ActivityNotFoundException e) {
            Toast.makeText(activity, R.string.no_file_picker, Toast.LENGTH_LONG).show();
            return false;
        }
    }

    static void reset(Context ctx) {
        deleteRecursive(new File(ctx.getFilesDir(), "gpu_driver"));
    }

    static void install(Context ctx, Uri uri, Callback cb) {
        Handler ui = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            File base = new File(ctx.getFilesDir(), "gpu_driver");
            File dest = new File(base, "custom");
            try {
                deleteRecursive(base);
                if (!dest.mkdirs() && !dest.isDirectory()) throw new Exception("mkdir failed");
                String srcName = queryName(ctx, uri);
                boolean isSo = srcName != null && srcName.toLowerCase().endsWith(".so");
                if (isSo) {
                    File outFile = new File(dest, sanitizeName(srcName));
                    try (InputStream in = ctx.getContentResolver().openInputStream(uri);
                         FileOutputStream out = new FileOutputStream(outFile)) {
                        if (in == null) throw new Exception("cannot open file");
                        copy(in, out);
                    }
                    String dir = dest.getAbsolutePath() + "/";
                    String lib = outFile.getName();
                    ui.post(() -> cb.done(true, dir, lib, lib, null));
                    return;
                }
                // Extract ZIP safely (guard against zip-slip)
                String canonicalDest = dest.getCanonicalPath() + File.separator;
                try (InputStream raw = ctx.getContentResolver().openInputStream(uri);
                     ZipInputStream zis = raw != null ? new ZipInputStream(raw) : null) {
                    if (zis == null) throw new Exception("cannot open zip");
                    byte[] buf = new byte[16384];
                    ZipEntry entry;
                    while ((entry = zis.getNextEntry()) != null) {
                        if (entry.isDirectory()) continue;
                        File out = new File(dest, entry.getName());
                        String canonicalOut = out.getCanonicalPath();
                        if (!canonicalOut.startsWith(canonicalDest)) {
                            throw new SecurityException("invalid zip path");
                        }
                        File parent = out.getParentFile();
                        if (parent != null) parent.mkdirs();
                        try (FileOutputStream fos = new FileOutputStream(out)) {
                            int n;
                            while ((n = zis.read(buf)) > 0) fos.write(buf, 0, n);
                        }
                        zis.closeEntry();
                    }
                }
                // Resolve libraryName and display name from meta.json if present
                String lib = null;
                String displayName = null;
                File meta = findFile(dest, "meta.json");
                if (meta != null) {
                    String json = readText(meta);
                    JSONObject obj = new JSONObject(json);
                    String libName = obj.optString("libraryName", "");
                    if (!libName.isEmpty()) lib = libName;
                    String name = obj.optString("name", "");
                    String ver = obj.optString("driverVersion", "");
                    if (!name.isEmpty()) {
                        displayName = ver.isEmpty() ? name : (name + " (" + ver + ")");
                    }
                }
                File libFile = lib != null ? findFile(dest, lib) : findFirstSo(dest);
                if (libFile == null || !libFile.isFile()) {
                    throw new Exception("no .so driver library found");
                }
                String dir = libFile.getParentFile().getAbsolutePath() + "/";
                String finalLib = libFile.getName();
                String finalName = displayName != null ? displayName : (srcName != null ? srcName : finalLib);
                ui.post(() -> cb.done(true, dir, finalLib, finalName, null));
            } catch (Exception e) {
                deleteRecursive(base);
                String msg = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
                ui.post(() -> cb.done(false, "", "", "", msg));
            }
        }).start();
    }

    private static String queryName(Context ctx, Uri uri) {
        try (Cursor c = ctx.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {}
        return null;
    }

    private static String sanitizeName(String s) {
        int slash = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\'));
        return slash >= 0 ? s.substring(slash + 1) : s;
    }

    private static void copy(InputStream in, FileOutputStream out) throws Exception {
        byte[] buf = new byte[16384];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
    }

    private static String readText(File f) throws Exception {
        try (FileInputStream in = new FileInputStream(f);
             ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private static File findFile(File dir, String name) {
        File[] list = dir.listFiles();
        if (list == null) return null;
        for (File f : list) {
            if (f.isDirectory()) {
                File found = findFile(f, name);
                if (found != null) return found;
            } else if (f.getName().equalsIgnoreCase(name)) {
                return f;
            }
        }
        return null;
    }

    private static File findFirstSo(File dir) {
        File[] list = dir.listFiles();
        if (list == null) return null;
        for (File f : list) {
            if (f.isDirectory()) {
                File found = findFirstSo(f);
                if (found != null) return found;
            } else if (f.getName().toLowerCase().endsWith(".so")) {
                return f;
            }
        }
        return null;
    }

    private static void deleteRecursive(File f) {
        if (f == null || !f.exists()) return;
        File[] list = f.listFiles();
        if (list != null) {
            for (File c : list) deleteRecursive(c);
        }
        f.delete();
    }
}
