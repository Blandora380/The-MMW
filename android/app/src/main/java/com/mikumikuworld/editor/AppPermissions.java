package com.mikumikuworld.editor;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.PowerManager;
import android.provider.Settings;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/** User-approved setup for shared files and exports that continue outside the editor. */
final class AppPermissions {
    private static final int FILES_REQUEST = 4108;
    private static final int NOTIFICATIONS_REQUEST = 4109;
    private final Activity activity;
    private AlertDialog dialog;
    private Button filesButton;
    private Button notificationsButton;
    private Button backgroundButton;

    AppPermissions(Activity activity) {
        this.activity = activity;
    }

    void onResume() {
        refresh();
        if (!activity.getPreferences(Activity.MODE_PRIVATE).getBoolean("permissions_setup_shown", false)) {
            activity.getWindow().getDecorView().post(this::show);
        }
    }

    void show() {
        if (activity.isFinishing() || activity.isDestroyed() || (dialog != null && dialog.isShowing()))
            return;
        activity.getPreferences(Activity.MODE_PRIVATE).edit().putBoolean("permissions_setup_shown", true).apply();
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        content.setPadding(padding, padding / 2, padding, padding / 2);
        addText(content, "Enable these permissions to access shared chart files and track video exports while using other apps. You can return here from Settings > Preview > Android permissions.");
        filesButton = addButton(content, this::requestFiles);
        addText(content, "All files: read and save charts, audio, images, and video in shared storage. Android still protects other apps' private folders.");
        notificationsButton = addButton(content, this::requestNotifications);
        addText(content, "Notifications: show the export percentage, frame count, and finalizing status.");
        backgroundButton = addButton(content, this::requestBackground);
        addText(content, "Background export: allow the app to keep working when you switch apps or turn off the screen. Background processing is active only during an export.");
        addText(content, "Some phones also have an Allow background activity or Unrestricted option under App info > Battery usage. Enable it there if your phone pauses exports.");
        Button appInfo = addButton(content, () -> openSettings(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, packageUri())));
        appInfo.setText("App info / battery settings");
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(content);
        dialog = new AlertDialog.Builder(activity)
                .setTitle("Files and background export")
                .setView(scroll)
                .setPositiveButton("Done", (whichDialog, which) -> { })
                .create();
        dialog.show();
        refresh();
    }

    private void addText(LinearLayout parent, String value) {
        TextView text = new TextView(activity);
        text.setText(value);
        text.setTextSize(14);
        text.setPadding(0, 8, 0, 12);
        parent.addView(text);
    }

    private Button addButton(LinearLayout parent, Runnable action) {
        Button button = new Button(activity);
        button.setAllCaps(false);
        button.setOnClickListener(view -> action.run());
        parent.addView(button, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return button;
    }

    private Uri packageUri() {
        return Uri.parse("package:" + activity.getPackageName());
    }

    private boolean filesAllowed() {
        if (Build.VERSION.SDK_INT >= 30)
            return Environment.isExternalStorageManager();
        return activity.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && activity.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean notificationsAllowed() {
        NotificationManager manager = activity.getSystemService(NotificationManager.class);
        NotificationChannel channel = manager.getNotificationChannel(VideoExportService.CHANNEL_ID);
        return manager.areNotificationsEnabled()
                && (channel == null || channel.getImportance() != NotificationManager.IMPORTANCE_NONE);
    }

    private boolean backgroundAllowed() {
        return activity.getSystemService(PowerManager.class).isIgnoringBatteryOptimizations(activity.getPackageName());
    }

    void refresh() {
        if (dialog == null || !dialog.isShowing())
            return;
        boolean files = filesAllowed();
        filesButton.setText(files ? "All files access: allowed" : "Allow access to all files");
        filesButton.setEnabled(!files);
        boolean notifications = notificationsAllowed();
        notificationsButton.setText(notifications ? "Export notifications: allowed" : "Allow export notifications");
        notificationsButton.setEnabled(!notifications);
        boolean background = backgroundAllowed();
        backgroundButton.setText(background ? "Background export: allowed" : "Allow app to run in the background");
        backgroundButton.setEnabled(!background);
    }

    private void requestFiles() {
        if (Build.VERSION.SDK_INT >= 30) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, packageUri());
            try {
                activity.startActivity(intent);
            } catch (Exception error) {
                openSettings(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else if (activity.getPreferences(Activity.MODE_PRIVATE).getBoolean("files_requested", false)
                && !activity.shouldShowRequestPermissionRationale(Manifest.permission.WRITE_EXTERNAL_STORAGE)) {
            openSettings(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, packageUri()));
        } else {
            activity.getPreferences(Activity.MODE_PRIVATE).edit().putBoolean("files_requested", true).apply();
            activity.requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE }, FILES_REQUEST);
        }
    }

    private void requestNotifications() {
        boolean previouslyRequested = activity.getPreferences(Activity.MODE_PRIVATE).getBoolean("notifications_requested", false);
        if (Build.VERSION.SDK_INT >= 33
                && activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
                && (!previouslyRequested || activity.shouldShowRequestPermissionRationale(Manifest.permission.POST_NOTIFICATIONS))) {
            activity.getPreferences(Activity.MODE_PRIVATE).edit().putBoolean("notifications_requested", true).apply();
            activity.requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, NOTIFICATIONS_REQUEST);
        } else {
            Intent intent = new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS);
            intent.putExtra(Settings.EXTRA_APP_PACKAGE, activity.getPackageName());
            openSettings(intent);
        }
    }

    private void requestBackground() {
        openSettings(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, packageUri()));
    }

    private void openSettings(Intent intent) {
        try {
            activity.startActivity(intent);
        } catch (Exception error) {
            try {
                activity.startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, packageUri()));
            } catch (Exception unavailable) {
                Toast.makeText(activity, "Open this app's permissions in Android Settings.", Toast.LENGTH_LONG).show();
            }
        }
    }

    void close() {
        if (dialog != null)
            dialog.dismiss();
    }
}
