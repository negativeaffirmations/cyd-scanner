package io.github.negativeaffirmations.cointelflo;

import android.os.Bundle;

import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        registerPlugin(CfloFilesPlugin.class);   // app's own plugin: saves downloads to Download/COINTEL-FLO/
        registerPlugin(CfloNotifyPlugin.class);  // alert sounds, vibration patterns, lock-screen visibility
        super.onCreate(savedInstanceState);
    }
}
