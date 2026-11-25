package com.racas.mostwanted;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.List;

// Minimal read-only ContentProvider for sharing exported .zip/.log files without androidx.core.
public final class LogFileProvider extends ContentProvider {

    private static File rootForName(Context ctx, String name) {
        if ("cache".equals(name)) return ctx.getCacheDir();
        if ("appfiles".equals(name)) return ctx.getExternalFilesDir(null);
        if ("external".equals(name)) return Environment.getExternalStorageDirectory();
        return null;
    }

    public static Uri getUriForFile(Context ctx, File file) {
        try {
            String target = file.getCanonicalPath();
            String[] names = {"cache", "appfiles", "external"};
            for (String name : names) {
                File root = rootForName(ctx, name);
                if (root == null) continue;
                String base = root.getCanonicalPath();
                String prefix = base.endsWith(File.separator) ? base : base + File.separator;
                if (target.startsWith(prefix)) {
                    String rel = target.substring(prefix.length()).replace(File.separatorChar, '/');
                    return new Uri.Builder()
                            .scheme("content")
                            .authority(ctx.getPackageName() + ".fileprovider")
                            .appendPath(name)
                            .appendEncodedPath(Uri.encode(rel, "/"))
                            .build();
                }
            }
        } catch (IOException e) {
            throw new IllegalArgumentException("Cannot resolve file: " + file, e);
        }
        throw new IllegalArgumentException("File outside allowed roots: " + file);
    }

    private File resolve(Uri uri) throws FileNotFoundException {
        Context ctx = getContext();
        if (ctx == null) throw new FileNotFoundException("No context");
        List<String> segs = uri.getPathSegments();
        if (segs.size() < 2) throw new FileNotFoundException("Invalid URI: " + uri);
        File root = rootForName(ctx, segs.get(0));
        if (root == null) throw new FileNotFoundException("Unknown root: " + segs.get(0));
        StringBuilder rel = new StringBuilder();
        for (int i = 1; i < segs.size(); i++) {
            if (i > 1) rel.append(File.separatorChar);
            rel.append(segs.get(i));
        }
        try {
            String base = root.getCanonicalPath();
            String prefix = base.endsWith(File.separator) ? base : base + File.separator;
            File candidate = new File(root, rel.toString()).getCanonicalFile();
            String path = candidate.getPath();
            if (!path.startsWith(prefix)) throw new FileNotFoundException("Path traversal blocked");
            String lower = candidate.getName().toLowerCase();
            if (!lower.endsWith(".zip") && !lower.endsWith(".log")) {
                throw new FileNotFoundException("Disallowed extension");
            }
            return candidate;
        } catch (IOException e) {
            throw new FileNotFoundException("Resolve failed: " + e.getMessage());
        }
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        try {
            File file = resolve(uri);
            String[] cols = projection != null ? projection : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
            MatrixCursor cursor = new MatrixCursor(cols, 1);
            Object[] row = new Object[cols.length];
            for (int i = 0; i < cols.length; i++) {
                if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = file.getName();
                else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = file.length();
            }
            cursor.addRow(row);
            return cursor;
        } catch (FileNotFoundException e) {
            return new MatrixCursor(new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, 0);
        }
    }

    @Override
    public String getType(Uri uri) {
        String last = uri.getLastPathSegment();
        if (last != null) {
            String lower = last.toLowerCase();
            if (lower.endsWith(".zip")) return "application/zip";
            if (lower.endsWith(".log")) return "text/plain";
        }
        return "application/octet-stream";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("Read-only provider");
        return ParcelFileDescriptor.open(resolve(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Read-only provider");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Read-only provider");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Read-only provider");
    }
}
