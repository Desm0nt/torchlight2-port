package com.xiaoboluo.d2r;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

// Restart of the game (DmActivity.restartGame, e.g. after the TL2 mod list changed): runs in its own process (":restart" in the manifest),
// started by the visible game just before it quits; waits until the game process is gone, then opens DmActivity again and ends itself.
public final class RestartActivity extends Activity {
    static final String EXTRA_PID = "pid";
    private int left = 100;   // 100 x 50 ms = 5 s at most

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() { public void run() {
            if (pid > 0 && new java.io.File("/proc/" + pid).exists() && --left > 0) { h.postDelayed(this, 50); return; }
            startActivity(new Intent(RestartActivity.this, DmActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
            finish(); overridePendingTransition(0, 0);
            h.postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
        }});
    }
}
