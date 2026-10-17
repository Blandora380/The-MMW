package com.mikumikuworld.editor;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import java.util.Locale;

public final class VideoExportService extends Service {
    static final String CHANNEL_ID = "video_export";
    private static final int NOTIFICATION_ID = 31;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static VideoExportService runningService;
    private static long completedFrames;
    private static long totalFrames;
    private static boolean isFinalizing;
    private PowerManager.WakeLock exportWakeLock;

    static void resetProgress() {
        synchronized (VideoExportService.class) {
            completedFrames = 0;
            totalFrames = 0;
            isFinalizing = false;
        }
    }

    static void updateProgress(long frames, long total, boolean finalizing) {
        synchronized (VideoExportService.class) {
            completedFrames = Math.max(0, frames);
            totalFrames = Math.max(0, total);
            isFinalizing = finalizing;
        }
        MAIN.post(() -> {
            if (runningService != null)
                runningService.getSystemService(NotificationManager.class)
                        .notify(NOTIFICATION_ID, runningService.buildNotification());
        });
    }

    @Override
    public void onCreate() {
        super.onCreate();
        runningService = this;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "Video export", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Keeps chart video export running while you use other apps");
            getSystemService(NotificationManager.class).createNotificationChannel(channel);
        }
        PowerManager power = getSystemService(PowerManager.class);
        exportWakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, getPackageName() + ":video_export");
        exportWakeLock.setReferenceCounted(false);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification notification = buildNotification();
        if (Build.VERSION.SDK_INT >= 35)
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROCESSING);
        else if (Build.VERSION.SDK_INT >= 29)
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        else
            startForeground(NOTIFICATION_ID, notification);
        // Bound the lock to the platform's maximum media-processing window.
        // It is also released immediately when export finishes or is cancelled.
        exportWakeLock.acquire(6L * 60L * 60L * 1000L);
        return START_NOT_STICKY;
    }

    private Notification buildNotification() {
        long frames;
        long total;
        boolean finalizing;
        synchronized (VideoExportService.class) {
            frames = completedFrames;
            total = totalFrames;
            finalizing = isFinalizing;
        }
        int percent = total > 0 ? (int)Math.min(99, (double)frames / total * 100.0) : 0;
        String description = finalizing ? "Finalizing and saving MP4..."
                : total > 0 ? String.format(Locale.ROOT, "%d%% - %,d / %,d frames", percent, frames, total)
                : "Preparing export...";
        Intent openApp = new Intent(this, MainActivity.class);
        openApp.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent pendingIntent = PendingIntent.getActivity(this, 0, openApp,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        return builder
                .setSmallIcon(android.R.drawable.stat_sys_upload)
                .setContentTitle("Exporting video")
                .setContentText(description)
                .setSubText("Export continues in the background")
                .setProgress(100, percent, finalizing || total <= 0)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(pendingIntent)
                .build();
    }

    @Override
    public void onDestroy() {
        runningService = null;
        if (exportWakeLock != null && exportWakeLock.isHeld())
            exportWakeLock.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
