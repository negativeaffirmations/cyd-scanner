package io.github.negativeaffirmations.cointelflo;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import android.os.PowerManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Background diagnostics (docs/android-app.md "Stage 2"): a small text log in the app's private storage of what
 * happens while the app runs in the background, so a walk with the screen off shows which part stopped: the app
 * process (heartbeat gaps), the page's JavaScript (ticks sent but not answered), the scanner link (no BLE / DETS
 * packets) or the alerts (DETS arriving, nothing shown). Counts and phone state only: no MACs, names or positions.
 * Two files of up to 256 KB each (bgdiag.log + bgdiag.1.log).
 */
final class CfloDiag {
    private static final Object LOCK = new Object();
    private static final long MAX = 256 * 1024;
    private static final SimpleDateFormat FMT = new SimpleDateFormat("MM-dd HH:mm:ss", Locale.US);

    // Counters, read and reset by the service heartbeat.
    static volatile long ticksSent = 0, acks = 0, fixes = 0, lastAckAt = 0;
    static volatile String js = "";   // the page's own counters from its last answer

    private CfloDiag() {}

    private static File file(Context c, boolean old) { return new File(c.getFilesDir(), old ? "bgdiag.1.log" : "bgdiag.log"); }

    static void log(Context c, String msg) {
        if (c == null) return;
        Context app = c.getApplicationContext();
        String line;
        synchronized (FMT) { line = FMT.format(new Date()) + " " + msg + "\n"; }
        synchronized (LOCK) {
            try {
                File f = file(app, false);
                if (f.length() > MAX) {
                    File o = file(app, true);
                    if (o.exists()) o.delete();
                    f.renameTo(o);
                    f = file(app, false);
                }
                try (FileOutputStream out = new FileOutputStream(f, true)) { out.write(line.getBytes(StandardCharsets.UTF_8)); }
            } catch (Exception e) { /* diagnostics must never break the app */ }
        }
    }

    /** The phone's power state in one short string. */
    static String phoneState(Context c) {
        StringBuilder s = new StringBuilder();
        try {
            PowerManager pm = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                s.append("screen=").append(pm.isInteractive() ? "on" : "off");
                s.append(" doze=").append(pm.isDeviceIdleMode() ? 1 : 0);
                if (Build.VERSION.SDK_INT >= 33) s.append(" lightdoze=").append(pm.isDeviceLightIdleMode() ? 1 : 0);
                s.append(" saver=").append(pm.isPowerSaveMode() ? 1 : 0);
                s.append(" exempt=").append(pm.isIgnoringBatteryOptimizations(c.getPackageName()) ? 1 : 0);
            }
            ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
            if (am != null) s.append(" bgRestricted=").append(am.isBackgroundRestricted() ? 1 : 0);
        } catch (Exception e) { s.append(" state?"); }
        return s.toString();
    }

    static String read(Context c, int maxBytes) {
        Context app = c.getApplicationContext();
        synchronized (LOCK) {
            StringBuilder sb = new StringBuilder();
            for (File f : new File[] { file(app, true), file(app, false) }) {
                if (!f.exists()) continue;
                try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
                    byte[] b = new byte[(int) r.length()];
                    r.readFully(b);
                    sb.append(new String(b, StandardCharsets.UTF_8));
                } catch (Exception e) { /* skip */ }
            }
            if (sb.length() > maxBytes) {
                int cut = sb.length() - maxBytes;
                int nl = sb.indexOf("\n", cut);
                return sb.substring(nl >= 0 ? nl + 1 : cut);
            }
            return sb.toString();
        }
    }

    static void clear(Context c) {
        Context app = c.getApplicationContext();
        synchronized (LOCK) {
            file(app, false).delete();
            file(app, true).delete();
        }
    }
}
