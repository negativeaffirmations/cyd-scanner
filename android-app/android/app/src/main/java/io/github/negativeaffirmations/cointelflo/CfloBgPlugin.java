package io.github.negativeaffirmations.cointelflo;

import android.Manifest;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;

import androidx.activity.result.ActivityResult;
import androidx.core.content.ContextCompat;

import com.getcapacitor.JSObject;
import com.getcapacitor.PermissionState;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.ActivityCallback;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.getcapacitor.annotation.Permission;
import com.getcapacitor.annotation.PermissionCallback;

/**
 * Stage 2 background support for the page (android-app/native/shim-background.js; docs/android-app.md "Stage 2").
 *
 *   start({title, text})  start / update the foreground service (CfloBgService) and its notification
 *   stop({lost?: text})   stop it; with `lost`, leave a dismissible "link lost" note
 *   status() -> {running, batteryExempt, location}
 *   requestBatteryExemption() -> status()    Android's "let the app run in the background" prompt
 *   startLocation() / stopLocation()         native GPS (the web view stops geolocation while the page is hidden)
 *   events: "location" {lat, lon, acc, alt?, spd?, hdg?, time}, "tick" (every 1 s while the app is in the background
 *           and the service runs: the hidden page's own timers are throttled), "disconnectRequested"
 */
@CapacitorPlugin(name = "CfloBg", permissions = {
        @Permission(alias = "location", strings = { Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION })
})
public class CfloBgPlugin extends Plugin {
    private static final String TAG = "CfloBg";
    private static final long TICK_MS = 1000;
    private static final long NET_AFTER_GPS_MS = 5000;   // a network fix is used only when GPS has been quiet this long

    private final Handler main = new Handler(Looper.getMainLooper());
    private boolean inBackground = false;
    private LocationManager lm;
    private LocationListener gpsL, netL;
    private long lastGpsAt = 0;

    private final Runnable onDisconnect = () -> main.post(() -> notifyListeners("disconnectRequested", new JSObject()));

    private final Runnable onServiceStopped = () -> main.post(() -> {
        try {
            if (getBridge() != null && getBridge().getWebView() instanceof CfloWebView)
                ((CfloWebView) getBridge().getWebView()).serviceStopped();
        } catch (Exception e) { Log.w(TAG, "serviceStopped: " + e); }
    });

    @Override
    public void load() {
        CfloBgService.onDisconnectRequest = onDisconnect;
        CfloBgService.onStopped = onServiceStopped;
    }

    private void appState(boolean fg) {
        JSObject o = new JSObject();
        o.put("fg", fg);
        notifyListeners("appState", o);
    }

    // ---------- foreground service ----------
    @PluginMethod
    public void start(PluginCall call) {
        Context ctx = getContext();
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null) nm.cancel(CfloBgService.LOST_ID);
        Intent i = new Intent(ctx, CfloBgService.class).setAction(CfloBgService.ACTION_START)
                .putExtra("title", call.getString("title", "COINTEL-FLO"))
                .putExtra("text", call.getString("text", "Connected to a scanner."));
        CfloBgService.startPending = true;
        try {
            ContextCompat.startForegroundService(ctx, i);
        } catch (Exception e) {   // Android 12+: refused when the app is in the background
            CfloBgService.startPending = false;
            CfloDiag.log(ctx, "service start refused: " + e.getClass().getSimpleName());
            call.reject("Could not start the background service: " + e.getMessage());
            return;
        }
        if (inBackground) { main.removeCallbacks(tick); main.postDelayed(tick, TICK_MS); }
        call.resolve();
    }

    @PluginMethod
    public void stop(PluginCall call) {
        stopWhenSafe(call.getString("lost", null), 0);
        call.resolve();
    }

    /** stopService only once a pending start reached startForeground (else Android kills the app); waits up to ~6 s. */
    private void stopWhenSafe(String lost, int tries) {
        if (tries == 0 && CfloBgService.running) CfloDiag.log(getContext(), "service stop requested" + (lost != null && !lost.isEmpty() ? " (link lost)" : ""));
        if (CfloBgService.startPending && tries < 30) {
            main.postDelayed(() -> stopWhenSafe(lost, tries + 1), 200);
            return;
        }
        Context ctx = getContext().getApplicationContext();   // may run after the activity was destroyed
        try { ctx.stopService(new Intent(ctx, CfloBgService.class)); } catch (Exception e) { Log.w(TAG, "stopService: " + e); }
        CfloBgService.running = false;
        if (lost != null && !lost.isEmpty()) {
            NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
            try {
                if (nm != null) nm.notify(CfloBgService.LOST_ID, CfloBgService.build(ctx, "COINTEL-FLO", lost, false));
            } catch (Exception e) { Log.w(TAG, "lost note: " + e); }   // no notification permission: nothing to show
        }
    }

    private JSObject statusObj() {
        JSObject r = new JSObject();
        r.put("running", CfloBgService.running);
        r.put("fg", !inBackground);
        PowerManager pm = (PowerManager) getContext().getSystemService(Context.POWER_SERVICE);
        r.put("batteryExempt", pm != null && pm.isIgnoringBatteryOptimizations(getContext().getPackageName()));
        r.put("location", hasLocation());
        return r;
    }

    @PluginMethod
    public void status(PluginCall call) { call.resolve(statusObj()); }

    // ---------- diagnostics (CfloDiag) ----------
    /** The page answers each background tick: proves its JavaScript runs, and carries its own counters. */
    @PluginMethod
    public void ack(PluginCall call) {
        CfloDiag.acks++;
        CfloDiag.lastAckAt = SystemClock.elapsedRealtime();
        String c = call.getString("c", "");
        CfloDiag.js = c.length() > 200 ? c.substring(0, 200) : c;
        call.resolve();
    }

    /** A line from the page (connect / disconnect / alert errors); counts and states only. */
    @PluginMethod
    public void diagNote(PluginCall call) {
        String m = call.getString("m", "");
        CfloDiag.log(getContext(), "page " + (m.length() > 200 ? m.substring(0, 200) : m));
        call.resolve();
    }

    @PluginMethod
    public void diagLog(PluginCall call) {
        JSObject r = new JSObject();
        r.put("text", CfloDiag.read(getContext(), 400 * 1024));
        r.put("state", CfloDiag.phoneState(getContext()));
        call.resolve(r);
    }

    @PluginMethod
    public void diagClear(PluginCall call) {
        CfloDiag.clear(getContext());
        CfloDiag.log(getContext(), "log cleared " + CfloDiag.phoneState(getContext()));
        call.resolve();
    }

    @PluginMethod
    public void requestBatteryExemption(PluginCall call) {
        Context ctx = getContext();
        Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + ctx.getPackageName()));
        try {
            startActivityForResult(call, i, "batteryDone");
            return;
        } catch (Exception e) { Log.w(TAG, "battery prompt: " + e); }
        try {   // a phone without the prompt: its app-info page (Battery -> Unrestricted)
            startActivityForResult(call, new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                    Uri.parse("package:" + ctx.getPackageName())), "batteryDone");
        } catch (Exception e) {
            call.reject("This phone has no battery setting for apps");
        }
    }

    @ActivityCallback
    private void batteryDone(PluginCall call, ActivityResult result) {
        if (call != null) call.resolve(statusObj());
    }

    // ---------- 1 s heartbeat while the app is in the background ----------
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (!inBackground || !(CfloBgService.running || CfloBgService.startPending)) return;   // start() restarts it
            if (CfloBgService.running) { CfloDiag.ticksSent++; notifyListeners("tick", new JSObject()); }
            main.postDelayed(this, TICK_MS);
        }
    };

    @Override
    protected void handleOnStop() {
        CfloDiag.log(getContext(), "app -> background (service " + (CfloBgService.running ? "on" : "off") + ") " + CfloDiag.phoneState(getContext()));
        appState(false);
        inBackground = true;
        main.removeCallbacks(tick);
        main.postDelayed(tick, TICK_MS);
    }

    @Override
    protected void handleOnStart() {
        if (inBackground) CfloDiag.log(getContext(), "app -> foreground");
        appState(true);
        inBackground = false;
        main.removeCallbacks(tick);
    }

    @Override
    protected void handleOnDestroy() {
        main.removeCallbacks(tick);
        stopUpdates();
        // REV-002: the page that owned the link is gone; a new page starts with no link, so the service must not
        // outlive it (wake lock + a "Connected" notification with nothing behind it).
        stopWhenSafe(null, 0);
        if (CfloBgService.onDisconnectRequest == onDisconnect) CfloBgService.onDisconnectRequest = null;
        if (CfloBgService.onStopped == onServiceStopped) CfloBgService.onStopped = null;
    }

    // ---------- native location ----------
    private boolean hasLocation() {
        Context c = getContext();
        return ContextCompat.checkSelfPermission(c, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
                || ContextCompat.checkSelfPermission(c, Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    @PluginMethod
    public void startLocation(PluginCall call) {
        if (!hasLocation()) {
            if (getPermissionState("location") == PermissionState.DENIED) { call.reject("Location permission denied", "denied"); return; }
            requestPermissionForAlias("location", call, "locationPerm");
            return;
        }
        beginUpdates(call);
    }

    @PermissionCallback
    private void locationPerm(PluginCall call) {
        if (hasLocation()) beginUpdates(call);
        else call.reject("Location permission denied", "denied");
    }

    private void send(Location l, boolean gps, boolean cached) {
        long now = System.currentTimeMillis();
        if (gps && !cached) lastGpsAt = SystemClock.elapsedRealtime();   // an old cached fix must not mute network fixes
        else if (SystemClock.elapsedRealtime() - lastGpsAt < NET_AFTER_GPS_MS) return;   // GPS is live: skip coarse fixes
        // Fix time on the phone's clock from its age (GNSS time vs a skewed phone clock would make it look old).
        long ageMs = (SystemClock.elapsedRealtimeNanos() - l.getElapsedRealtimeNanos()) / 1000000L;
        JSObject o = new JSObject();
        o.put("lat", l.getLatitude());
        o.put("lon", l.getLongitude());
        o.put("acc", l.hasAccuracy() ? l.getAccuracy() : 9999);
        if (l.hasAltitude()) o.put("alt", l.getAltitude());
        if (l.hasSpeed()) o.put("spd", l.getSpeed());
        if (l.hasBearing()) o.put("hdg", l.getBearing());
        o.put("time", now - Math.max(0, ageMs));
        CfloDiag.fixes++;
        notifyListeners("location", o);
    }

    private void beginUpdates(PluginCall call) {
        if (lm == null) lm = (LocationManager) getContext().getSystemService(Context.LOCATION_SERVICE);
        if (lm == null) { call.reject("No location on this phone", "unavailable"); return; }
        boolean gpsOn = false, netOn = false;
        try { gpsOn = lm.isProviderEnabled(LocationManager.GPS_PROVIDER); } catch (Exception e) {}
        try { netOn = lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER); } catch (Exception e) {}
        if (!gpsOn && !netOn) { call.reject("Location is turned off on the phone", "unavailable"); return; }
        // Per provider (REV-011): a coarse-only grant refuses GPS but still allows network fixes.
        if (gpsOn && gpsL == null) {
            Listener g = new Listener(true);
            try { lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, g, Looper.getMainLooper()); gpsL = g; }
            catch (Exception e) { Log.w(TAG, "GPS provider: " + e); }
        }
        if (netOn && netL == null) {
            Listener n = new Listener(false);
            try { lm.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, 2000, 0, n, Looper.getMainLooper()); netL = n; }
            catch (Exception e) { Log.w(TAG, "network provider: " + e); }
        }
        if (gpsL == null && netL == null) { call.reject("Location permission denied", "denied"); return; }
        CfloDiag.log(getContext(), "gps on (gps=" + (gpsL != null ? 1 : 0) + " net=" + (netL != null ? 1 : 0) + ")");
        Location last = null;   // a recent cached fix gets the page going at once (it drops ones > 10 s old)
        boolean lastGps = gpsL != null;
        try { last = lm.getLastKnownLocation(lastGps ? LocationManager.GPS_PROVIDER : LocationManager.NETWORK_PROVIDER); } catch (Exception e) {}
        if (last != null) send(last, lastGps, true);
        call.resolve();
    }

    private void stopUpdates() {
        if (lm == null) return;
        try { if (gpsL != null) lm.removeUpdates(gpsL); } catch (Exception e) {}
        try { if (netL != null) lm.removeUpdates(netL); } catch (Exception e) {}
        gpsL = null;
        netL = null;
    }

    @PluginMethod
    public void stopLocation(PluginCall call) {
        stopUpdates();
        call.resolve();
    }

    private class Listener implements LocationListener {
        private final boolean gps;
        Listener(boolean gps) { this.gps = gps; }
        @Override public void onLocationChanged(Location l) { send(l, gps, false); }
        @Override public void onProviderEnabled(String p) {}
        @Override public void onProviderDisabled(String p) {}
        @Override public void onStatusChanged(String p, int s, Bundle b) {}
    }
}
