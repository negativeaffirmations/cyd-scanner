package io.github.negativeaffirmations.cointelflo;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.net.Uri;
import android.os.Environment;
import android.provider.MediaStore;
import android.util.Base64;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;

/**
 * Saves the web app's downloads into the phone's Downloads folder (Download/COINTEL-FLO/) through MediaStore, which
 * needs no storage permission on Android 10+. The page sends a file in base64 chunks: begin -> append... -> finish
 * (or abort). Used by android-app/native/shim-platform.js in place of the browser's <a download>. docs/android-app.md
 */
@CapacitorPlugin(name = "CfloFiles")
public class CfloFilesPlugin extends Plugin {
    private static final String FOLDER = Environment.DIRECTORY_DOWNLOADS + "/COINTEL-FLO";

    private static class Pending {
        Uri uri;
        OutputStream out;
        long bytes;
    }

    private final Map<String, Pending> open = new HashMap<>();
    private int nextId = 1;

    @PluginMethod
    public void begin(PluginCall call) {
        String name = safeName(call.getString("name", "download.bin"));
        String mime = call.getString("mime", "application/octet-stream");
        ContentResolver cr = getContext().getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.Downloads.DISPLAY_NAME, name);
        v.put(MediaStore.Downloads.MIME_TYPE, mime);
        v.put(MediaStore.Downloads.RELATIVE_PATH, FOLDER);
        v.put(MediaStore.Downloads.IS_PENDING, 1);
        try {
            Uri uri = cr.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
            if (uri == null) throw new Exception("could not create the file");
            Pending p = new Pending();
            p.uri = uri;
            p.out = cr.openOutputStream(uri, "w");
            if (p.out == null) {
                cr.delete(uri, null, null);
                throw new Exception("could not open the file");
            }
            String id;
            synchronized (open) {
                id = String.valueOf(nextId++);
                open.put(id, p);
            }
            JSObject r = new JSObject();
            r.put("id", id);
            call.resolve(r);
        } catch (Exception e) {
            call.reject("Save failed: " + e.getMessage());
        }
    }

    @PluginMethod
    public void append(PluginCall call) {
        Pending p = get(call);
        if (p == null) return;
        try {
            byte[] b = Base64.decode(call.getString("data", ""), Base64.DEFAULT);
            p.out.write(b);
            p.bytes += b.length;
            call.resolve();
        } catch (Exception e) {
            drop(call.getString("id"));
            call.reject("Save failed: " + e.getMessage());
        }
    }

    @PluginMethod
    public void finish(PluginCall call) {
        Pending p = get(call);
        if (p == null) return;
        String id = call.getString("id");
        try {
            p.out.close();
            ContentValues v = new ContentValues();
            v.put(MediaStore.Downloads.IS_PENDING, 0);
            getContext().getContentResolver().update(p.uri, v, null, null);
            synchronized (open) {
                open.remove(id);
            }
            JSObject r = new JSObject();
            r.put("bytes", p.bytes);
            r.put("folder", FOLDER);
            call.resolve(r);
        } catch (Exception e) {
            drop(id);
            call.reject("Save failed: " + e.getMessage());
        }
    }

    @PluginMethod
    public void abort(PluginCall call) {
        drop(call.getString("id"));
        call.resolve();
    }

    private Pending get(PluginCall call) {
        Pending p;
        synchronized (open) {
            p = open.get(call.getString("id", ""));
        }
        if (p == null) call.reject("No such save in progress");
        return p;
    }

    private void drop(String id) {
        Pending p;
        synchronized (open) {
            p = open.remove(id == null ? "" : id);
        }
        if (p == null) return;
        try { p.out.close(); } catch (Exception ignored) {}
        try { getContext().getContentResolver().delete(p.uri, null, null); } catch (Exception ignored) {}
    }

    /** A plain file name: no folders, no control characters. */
    private static String safeName(String n) {
        n = n.replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_").trim();
        if (n.isEmpty() || n.equals(".") || n.equals("..")) n = "download.bin";
        return n.length() > 120 ? n.substring(n.length() - 120) : n;
    }
}
