package io.github.negativeaffirmations.cointelflo;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import androidx.core.app.NotificationCompat;
import androidx.core.app.ServiceCompat;
import androidx.core.content.ContextCompat;

/**
 * Stage 2 (docs/android-app.md "Stage 2"): keeps the app running while a scanner is connected, so the web view's
 * BLE link, GPS feed, live upload and alert checks go on with the screen off or the app in the background.
 *
 * A foreground service of type connectedDevice (+ location when the app holds a location permission), a partial
 * wake lock, and one ongoing notification ("Connected to TOLUBA", tap = open the app, Disconnect button). Started and
 * stopped from the page (CfloBgPlugin, android-app/native/shim-background.js) when the first scanner connects / the
 * last one disconnects. stopWithTask (manifest): swiping the app away ends it.
 *
 * Intents: ACTION_START (title, text) starts or updates it; ACTION_DISCONNECT (the notification button) tells the page
 * to disconnect, then stops. The "link lost" note (LOST_ID) is posted by the plugin, not here: starting a service from
 * the background just to post it could be refused.
 */
public class CfloBgService extends Service {
    static final String ACTION_START = "cflo.bg.START";
    static final String ACTION_DISCONNECT = "cflo.bg.DISCONNECT";
    static final String CHANNEL = "cflo-link";
    static final int NOTIF_ID = 7301;
    static final int LOST_ID = 7302;
    private static final String TAG = "CfloBg";
    private static final long WAKE_MAX_MS = 12L * 60 * 60 * 1000;   // safety cap; renewed on every start

    static final String CHANNEL_LOST = "cflo-link-lost";   // default importance: "alerts are off now" must be noticed
    private static final long WAKE_RENEW_MS = 60L * 60 * 1000;

    static volatile boolean running = false;
    /** True from the plugin's startForegroundService() until onStartCommand ran: a stopService() in between would
     *  crash the app ("did not then call startForeground"), so the plugin waits for this (REV-003). */
    static volatile boolean startPending = false;
    private final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable renewWake = new Runnable() {   // the 12 h cap is a safety net, not a session limit
        @Override public void run() {
            if (!running || wake == null) return;
            try { wake.acquire(WAKE_MAX_MS); } catch (Exception e) { Log.w(TAG, "wake renew: " + e); }
            handler.postDelayed(this, WAKE_RENEW_MS);
        }
    };
    /** Set by CfloBgPlugin: run on the main thread when the notification's Disconnect is tapped. */
    static volatile Runnable onDisconnectRequest = null;

    private PowerManager.WakeLock wake;
    private int types = 0;            // foreground types the service runs with now (0 = not foreground yet)

    @Override
    public IBinder onBind(Intent intent) { return null; }

    static void ensureChannel(Context ctx) {
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null || nm.getNotificationChannel(CHANNEL) != null) return;
        NotificationChannel ch = new NotificationChannel(CHANNEL, "Scanner connection", NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Shown while COINTEL-FLO keeps a scanner connected in the background");
        ch.setShowBadge(false);
        ch.enableVibration(false);
        ch.setSound(null, null);
        nm.createNotificationChannel(ch);
    }

    static void ensureLostChannel(Context ctx) {
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null || nm.getNotificationChannel(CHANNEL_LOST) != null) return;
        NotificationChannel ch = new NotificationChannel(CHANNEL_LOST, "Scanner link lost", NotificationManager.IMPORTANCE_DEFAULT);
        ch.setDescription("When the scanner link drops while COINTEL-FLO is in the background (alerts stop with it)");
        nm.createNotificationChannel(ch);
    }

    private static PendingIntent openApp(Context ctx) {
        Intent i = new Intent(ctx, MainActivity.class);
        i.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        return PendingIntent.getActivity(ctx, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    /** ongoing = the service's own notification (with Disconnect); else a dismissible note (link lost). */
    static Notification build(Context ctx, String title, String text, boolean ongoing) {
        if (ongoing) ensureChannel(ctx); else ensureLostChannel(ctx);
        NotificationCompat.Builder b = new NotificationCompat.Builder(ctx, ongoing ? CHANNEL : CHANNEL_LOST)
                .setSmallIcon(R.drawable.ic_stat_cflo)
                .setColor(0xFFAA00AA)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new NotificationCompat.BigTextStyle().bigText(text))
                .setContentIntent(openApp(ctx))
                .setOnlyAlertOnce(true)
                .setShowWhen(!ongoing)
                .setCategory(ongoing ? NotificationCompat.CATEGORY_SERVICE : NotificationCompat.CATEGORY_STATUS)
                .setPriority(ongoing ? NotificationCompat.PRIORITY_LOW : NotificationCompat.PRIORITY_DEFAULT)
                .setOngoing(ongoing)
                .setAutoCancel(!ongoing);
        if (ongoing) {
            Intent d = new Intent(ctx, CfloBgService.class).setAction(ACTION_DISCONNECT);
            PendingIntent pd = PendingIntent.getService(ctx, 1, d, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            b.addAction(0, "Disconnect", pd);
            b.setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE);
        }
        return b.build();
    }

    private boolean granted(String perm) {
        return ContextCompat.checkSelfPermission(this, perm) == PackageManager.PERMISSION_GRANTED;
    }

    private int wantedTypes() {
        int t = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE;
        if (granted(Manifest.permission.ACCESS_FINE_LOCATION) || granted(Manifest.permission.ACCESS_COARSE_LOCATION))
            t |= ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION;
        return t;
    }

    /** startForeground with the wanted types; without location if Android refuses that (e.g. asked from the background). */
    private boolean goForeground(Notification n) {
        int want = wantedTypes();
        if (types != 0 && (want & ~types) == 0) {        // already foreground with everything needed: just repaint
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (nm != null) nm.notify(NOTIF_ID, n);
            return true;
        }
        int[] tries = (want & ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION) != 0
                ? new int[] { want, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE }
                : new int[] { want };
        for (int t : tries) {
            if (types != 0 && (t & ~types) == 0) {            // the fallback is what already runs: repaint only
                NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
                if (nm != null) nm.notify(NOTIF_ID, n);
                return true;
            }
            try {
                ServiceCompat.startForeground(this, NOTIF_ID, n, t);
                types = t;
                return true;
            } catch (Exception e) {   // SecurityException / ForegroundServiceStartNotAllowedException: never crash the app
                Log.w(TAG, "startForeground(" + t + ") refused: " + e);
            }
        }
        return types != 0;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;
        if (ACTION_DISCONNECT.equals(action)) {
            Runnable r = onDisconnectRequest;
            if (r != null) {
                try { r.run(); } catch (Exception e) { Log.w(TAG, "disconnect request: " + e); }
            }
            finish();
            return START_NOT_STICKY;
        }
        String title = intent != null ? intent.getStringExtra("title") : null;
        String text = intent != null ? intent.getStringExtra("text") : null;
        Notification n = build(this, title != null ? title : "COINTEL-FLO",
                text != null ? text : "Connected to a scanner.", true);
        boolean fg = goForeground(n);
        startPending = false;   // startForeground has run (or was refused): a stopService is safe from here
        if (!fg) {
            // startForegroundService() obliges a startForeground call; when every try was refused, stop at once.
            finish();
            return START_NOT_STICKY;
        }
        if (wake == null) {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "cflo:scanner-link");
                wake.setReferenceCounted(false);   // each start renews the timeout; one release ends it
            }
        }
        if (wake != null) {
            try { wake.acquire(WAKE_MAX_MS); } catch (Exception e) { Log.w(TAG, "wake lock: " + e); }
        }
        running = true;
        handler.removeCallbacks(renewWake);
        handler.postDelayed(renewWake, WAKE_RENEW_MS);
        return START_NOT_STICKY;   // a killed process has no web view to resume: don't restart empty
    }

    private void finish() {
        running = false;
        releaseWake();
        try { ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE); }
        catch (Exception e) { Log.w(TAG, "stopForeground: " + e); }
        types = 0;
        stopSelf();
    }

    private void releaseWake() {
        handler.removeCallbacks(renewWake);
        if (wake != null) {
            try { if (wake.isHeld()) wake.release(); } catch (Exception e) { /* already released by its timeout */ }
        }
    }

    @Override
    public void onDestroy() {
        running = false;
        releaseWake();
        super.onDestroy();
    }
}
