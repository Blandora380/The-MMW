package com.arthenica.smartexception.java;

import android.util.Log;

/**
 * Small compatibility surface required by the bundled FFmpegKit Java classes.
 * The FFmpegKit archive references this helper only for stack trace formatting
 * and root-package registration; Android's logger already provides the former.
 */
public final class Exceptions {
    private Exceptions() {}

    public static String getStackTraceString(Throwable throwable) {
        return Log.getStackTraceString(throwable);
    }

    public static void registerRootPackage(String packageName) {
        // Stack traces are left in Android's standard format.
    }
}
