package io.github.negativeaffirmations.cointelflo;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;

import com.getcapacitor.CapacitorWebView;

/**
 * Stage 2 (docs/android-app.md "Stage 2", owner walk 1): Chromium treats a web view whose window is not visible as a
 * hidden page and stops running it a minute or two later, so alerts piled up until the app was opened again (then
 * arrived in a burst). While the background service runs (a scanner is connected), this web view keeps telling
 * Chromium its window is visible, so the page goes on running with the screen off or the app in the background.
 * Without a service it behaves like the stock one (no cost when idle). The page learns the real app state from
 * CfloBgPlugin's "appState" event instead of document.visibilityState.
 *
 * Used through res/layout/capacitor_bridge_layout_main.xml, which overrides Capacitor's layout of the same name.
 */
public class CfloWebView extends CapacitorWebView {
    private int realVisibility = View.VISIBLE;

    public CfloWebView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        realVisibility = visibility;
        if (visibility != View.VISIBLE && CfloBgService.running) {
            CfloDiag.log(getContext(), "webview kept visible (window " + (visibility == View.GONE ? "gone" : "invisible") + ")");
            super.onWindowVisibilityChanged(View.VISIBLE);
            return;
        }
        super.onWindowVisibilityChanged(visibility);
    }

    /** The service stopped while the window is hidden: let Chromium see the real state now (stop running the page). */
    void serviceStopped() {
        if (realVisibility != View.VISIBLE) super.onWindowVisibilityChanged(realVisibility);
    }
}
