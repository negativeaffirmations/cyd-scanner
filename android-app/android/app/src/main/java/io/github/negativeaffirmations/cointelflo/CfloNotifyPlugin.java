package io.github.negativeaffirmations.cointelflo;

import android.app.Activity;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.provider.Settings;
import android.service.notification.StatusBarNotification;

import androidx.activity.result.ActivityResult;

import com.getcapacitor.JSArray;
import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.ActivityCallback;
import com.getcapacitor.annotation.CapacitorPlugin;

import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Alert sounds, vibration patterns and lock-screen visibility for the app's notifications (Settings -> Notifications,
 * webapp/js/features/notify.js; used by android-app/native/shim-platform.js).
 *
 * Android fixes a notification channel's sound, vibration and lock-screen visibility when the channel is created, so
 * every distinct style gets its own channel: id "cflo-<kind>-<hash>" (the page computes the hash). Creating one deletes
 * the kind's older channels, and the build-6 channel "cflo-alerts".
 *
 *   ensureChannel({id, kind, name, description, sound: "default" | "" (silent) | uri,
 *                  vibration: null (phone default) | [] (none) | [on, off, on, ...] ms, lockscreen: bool})
 *   pickSound({current, title}) -> {uri, title} | {cancelled: true}     (the phone's own notification-sound picker)
 *   soundTitle({uri}) -> {title}
 *   vibrate({pattern: [on, off, ...]}) / stopVibrate()                     (preview / the pattern recorder)
 */
@CapacitorPlugin(name = "CfloNotify")
public class CfloNotifyPlugin extends Plugin {
    private static final String LEGACY = "cflo-alerts";
    private static final int MAX_STEPS = 48;
    private static final long MAX_STEP_MS = 10000;

    private NotificationManager nm() {
        return (NotificationManager) getContext().getSystemService(Context.NOTIFICATION_SERVICE);
    }

    private static boolean safeKind(String k) {
        return k != null && k.matches("[a-z]{1,16}");
    }

    /** [on, off, on, ...] (as the page stores it) -> Android's [delay, on, off, ...]; null when unusable. */
    private static long[] waveform(JSArray a) {
        if (a == null) return null;
        int n = Math.min(a.length(), MAX_STEPS);
        if (n == 0) return null;
        long[] w = new long[n + 1];
        w[0] = 0;
        long sum = 0;
        for (int i = 0; i < n; i++) {
            long v = a.optLong(i, 0);
            w[i + 1] = Math.max(0, Math.min(MAX_STEP_MS, v));
            sum += w[i + 1];
        }
        return sum > 0 ? w : null;   // createWaveform throws on an all-zero pattern
    }

    @PluginMethod
    public void ensureChannel(PluginCall call) {
        String id = call.getString("id", "");
        String kind = call.getString("kind", "");
        if (!safeKind(kind) || !id.matches("cflo-[a-z]{1,16}-[0-9a-z]{1,16}")) {
            call.reject("bad channel id");
            return;
        }
        // A throw from a plugin method crashes the app (Capacitor rethrows it), so every Android call is guarded.
        try {
            makeChannel(call, id);
        } catch (Exception e) {
            call.reject("channel: " + e.getMessage());
            return;
        }
        pruneOld(id, kind);
        JSObject r = new JSObject();
        r.put("id", id);
        call.resolve(r);
    }

    private void makeChannel(PluginCall call, String id) {
        NotificationManager m = nm();
        if (m.getNotificationChannel(id) == null) {
            NotificationChannel ch = new NotificationChannel(id, call.getString("name", "Scanner alerts"),
                    NotificationManager.IMPORTANCE_HIGH);
            ch.setDescription(call.getString("description", ""));
            // Lock screen: PRIVATE = the phone's own lock-screen setting decides; SECRET = never on the lock screen.
            ch.setLockscreenVisibility(Boolean.TRUE.equals(call.getBoolean("lockscreen", false))
                    ? android.app.Notification.VISIBILITY_PRIVATE : android.app.Notification.VISIBILITY_SECRET);
            String sound = call.getString("sound", "default");
            AudioAttributes aa = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build();
            if (sound == null || sound.isEmpty()) ch.setSound(null, null);
            else if (sound.equals("default")) ch.setSound(Settings.System.DEFAULT_NOTIFICATION_URI, aa);
            else ch.setSound(Uri.parse(sound), aa);
            JSArray vib = null;
            try { vib = call.getArray("vibration", null); } catch (Exception ignored) {}
            if (vib == null) {
                ch.enableVibration(true);              // the phone's default pattern
            } else if (vib.length() == 0) {
                ch.enableVibration(false);
            } else {
                long[] w = waveform(vib);
                ch.enableVibration(w != null);
                if (w != null) ch.setVibrationPattern(w);
            }
            m.createNotificationChannel(ch);
        }
    }

    /**
     * Drop the kind's older styles (and build 6's single channel) so the phone's settings list stays short. Deleting a
     * channel also removes its notifications from the shade, so a channel that still shows one is kept for now and
     * goes on a later call, once the user has dismissed or opened it.
     */
    private void pruneOld(String id, String kind) {
        NotificationManager m = nm();
        String prefix = "cflo-" + kind + "-";
        Set<String> showing = new HashSet<>();
        try {
            for (StatusBarNotification sb : m.getActiveNotifications()) {
                String c = sb.getNotification().getChannelId();
                if (c != null) showing.add(c);
            }
        } catch (Exception e) {
            return;   // can't tell what is showing: delete nothing
        }
        List<NotificationChannel> all;
        try { all = m.getNotificationChannels(); } catch (Exception e) { return; }
        for (NotificationChannel c : all) {
            String cid = c.getId();
            if (cid.equals(id) || showing.contains(cid)) continue;
            if (!cid.startsWith(prefix) && !cid.equals(LEGACY)) continue;
            try { m.deleteNotificationChannel(cid); } catch (Exception ignored) {}
        }
    }

    @PluginMethod
    public void pickSound(PluginCall call) {
        Intent i = new Intent(RingtoneManager.ACTION_RINGTONE_PICKER);
        i.putExtra(RingtoneManager.EXTRA_RINGTONE_TYPE, RingtoneManager.TYPE_NOTIFICATION);
        i.putExtra(RingtoneManager.EXTRA_RINGTONE_SHOW_DEFAULT, true);
        i.putExtra(RingtoneManager.EXTRA_RINGTONE_SHOW_SILENT, true);
        i.putExtra(RingtoneManager.EXTRA_RINGTONE_DEFAULT_URI, Settings.System.DEFAULT_NOTIFICATION_URI);
        i.putExtra(RingtoneManager.EXTRA_RINGTONE_TITLE, call.getString("title", "Alert sound"));
        String cur = call.getString("current", "default");
        Uri existing = cur == null || cur.isEmpty() ? null
                : cur.equals("default") ? Settings.System.DEFAULT_NOTIFICATION_URI : Uri.parse(cur);
        i.putExtra(RingtoneManager.EXTRA_RINGTONE_EXISTING_URI, existing);
        try {
            startActivityForResult(call, i, "soundPicked");
        } catch (Exception e) {
            call.reject("This phone has no sound picker");
        }
    }

    @SuppressWarnings("deprecation")
    @ActivityCallback
    private void soundPicked(PluginCall call, ActivityResult result) {
        if (call == null) return;
        JSObject r = new JSObject();
        Intent data = result.getData();
        if (result.getResultCode() != Activity.RESULT_OK || data == null) {
            r.put("cancelled", true);
            call.resolve(r);
            return;
        }
        Uri u = Build.VERSION.SDK_INT >= 34   // the typed getter is buggy on Android 13 (AndroidX IntentCompat uses 34 too)
                ? data.getParcelableExtra(RingtoneManager.EXTRA_RINGTONE_PICKED_URI, Uri.class)
                : data.getParcelableExtra(RingtoneManager.EXTRA_RINGTONE_PICKED_URI);
        if (u == null) {
            r.put("uri", "");
            r.put("title", "Silent");
        } else if (u.equals(Settings.System.DEFAULT_NOTIFICATION_URI) || RingtoneManager.isDefault(u)) {
            r.put("uri", "default");
            r.put("title", "Phone default");
        } else {
            r.put("uri", u.toString());
            r.put("title", titleOf(u));
        }
        call.resolve(r);
    }

    private String titleOf(Uri u) {
        try {
            Ringtone rt = RingtoneManager.getRingtone(getContext(), u);
            String t = rt == null ? null : rt.getTitle(getContext());
            if (t != null && !t.isEmpty()) return t;
        } catch (Exception ignored) {}
        return "Custom sound";
    }

    @PluginMethod
    public void soundTitle(PluginCall call) {
        String s = call.getString("uri", "default");
        JSObject r = new JSObject();
        r.put("title", s == null || s.isEmpty() ? "Silent" : s.equals("default") ? "Phone default" : titleOf(Uri.parse(s)));
        call.resolve(r);
    }

    private Vibrator vibrator() {
        if (Build.VERSION.SDK_INT >= 31) {
            VibratorManager vm = (VibratorManager) getContext().getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            return vm == null ? null : vm.getDefaultVibrator();
        }
        return (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);
    }

    @PluginMethod
    public void vibrate(PluginCall call) {
        Vibrator v = vibrator();
        long[] w = null;
        try { w = waveform(call.getArray("pattern", null)); } catch (Exception ignored) {}
        if (v == null || !v.hasVibrator() || w == null) {
            call.resolve();
            return;
        }
        try {
            v.cancel();
            v.vibrate(VibrationEffect.createWaveform(w, -1));
        } catch (Exception ignored) {}
        call.resolve();
    }

    @PluginMethod
    public void stopVibrate(PluginCall call) {
        try {
            Vibrator v = vibrator();
            if (v != null) v.cancel();
        } catch (Exception ignored) {}
        call.resolve();
    }
}
