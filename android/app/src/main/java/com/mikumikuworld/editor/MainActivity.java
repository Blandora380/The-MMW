package com.mikumikuworld.editor;

import android.app.Activity;
import android.app.NativeActivity;
import android.media.AudioManager;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.InputMethodManager;
import android.content.Context;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.view.Gravity;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.text.InputType;
import android.util.Log;
import android.os.Handler;
import android.os.Looper;
import com.arthenica.ffmpegkit.FFmpegKit;
import com.arthenica.ffmpegkit.FFmpegSession;
import com.arthenica.ffmpegkit.ReturnCode;
import com.arthenica.ffmpegkit.SessionState;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public final class MainActivity extends NativeActivity {
    private static final int IMMERSIVE_FLAGS = View.SYSTEM_UI_FLAG_FULLSCREEN |
        View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
        View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
        View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
        View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN;
    private static final int PICKER_REQUEST = 4107;
    private long pickerId;
    private boolean pickerSave;
    private String pickerExtensions;
    private EditText imeTarget;
    private FrameLayout exportVideoOverlay;
    private WebView exportVideoWebView;
    private final Handler exportVideoHandler = new Handler(Looper.getMainLooper());
    private int exportVideoGeneration;
    private int exportVideoScrapeAttempts;
    private boolean scrapingExportChannel;
    private String lastExportVideoId;
    private boolean imeActive;
    private boolean clearingImeText;
    private AppPermissions appPermissions;
    private final Map<String, Uri> pendingExports = new HashMap<>();
    private final Map<String, Uri> importedDocuments = new HashMap<>();
    private final Map<Long, FFmpegSession> ffmpegExportSessions = new ConcurrentHashMap<>();

    private static final String PROJECT_SEKAI_VIDEOS = "https://www.youtube.com/@pj_sekai_colorfulstage/videos";

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        appPermissions = new AppPermissions(this);
        hideSystemBars();
        imeTarget = new EditText(this) {
            private boolean forwardsKey(int keyCode) {
                return keyCode == KeyEvent.KEYCODE_DEL || keyCode == KeyEvent.KEYCODE_ENTER ||
                    keyCode == KeyEvent.KEYCODE_DPAD_LEFT || keyCode == KeyEvent.KEYCODE_DPAD_RIGHT ||
                    keyCode == KeyEvent.KEYCODE_DPAD_UP || keyCode == KeyEvent.KEYCODE_DPAD_DOWN ||
                    keyCode == KeyEvent.KEYCODE_TAB || keyCode == KeyEvent.KEYCODE_ESCAPE;
            }
            @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
                if (forwardsKey(keyCode)) {
                    nativeKeyInput(keyCode, true);
                    return true;
                }
                return super.onKeyDown(keyCode, event);
            }
            @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
                if (forwardsKey(keyCode)) {
                    nativeKeyInput(keyCode, false);
                    return true;
                }
                return super.onKeyUp(keyCode, event);
            }
        };
        imeTarget.setSingleLine(true);
        imeTarget.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        imeTarget.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE |
            android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        imeTarget.setTextColor(Color.TRANSPARENT);
        imeTarget.setBackgroundColor(Color.TRANSPARENT);
        imeTarget.setAlpha(0.01f);
        imeTarget.setVisibility(View.GONE);
        imeTarget.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { }
            @Override public void afterTextChanged(Editable value) {
                if (clearingImeText || value.length() == 0)
                    return;
                String text = value.toString();
                clearingImeText = true;
                value.clear();
                clearingImeText = false;
                nativeTextInput(text);
            }
        });
        ViewGroup content = findViewById(android.R.id.content);
        content.addView(imeTarget, new ViewGroup.LayoutParams(1, 1));
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        if (appPermissions != null)
            appPermissions.onResume();
    }

    public void showAppPermissions() {
        runOnUiThread(() -> appPermissions.show());
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (appPermissions != null)
            appPermissions.refresh();
        hideSystemBars();
    }

    @Override
    protected void onDestroy() {
        if (appPermissions != null)
            appPermissions.close();
        hideExportVideoPlayerOnUiThread();
        stopService(new Intent(this, VideoExportService.class));
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemBars();
    }

    private void hideSystemBars() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                return;
            }
        }
        getWindow().getDecorView().setSystemUiVisibility(IMMERSIVE_FLAGS);
    }

    public void showExportVideoPlayer() {
        VideoExportService.resetProgress();
        runOnUiThread(() -> {
            startForegroundService(new Intent(this, VideoExportService.class));
            hideExportVideoPlayerOnUiThread();
            getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            final int generation = ++exportVideoGeneration;
            exportVideoScrapeAttempts = 0;
            scrapingExportChannel = true;

            FrameLayout overlay = new FrameLayout(this);
            overlay.setBackgroundColor(Color.rgb(18, 20, 28));
            overlay.setElevation(dp(12));
            FrameLayout.LayoutParams overlayParams = new FrameLayout.LayoutParams(
                    dp(360), dp(240), Gravity.BOTTOM | Gravity.RIGHT);
            overlayParams.setMargins(0, 0, dp(16), dp(16));

            TextView title = new TextView(this);
            title.setText("Project SEKAI video");
            title.setTextColor(Color.WHITE);
            title.setTextSize(14);
            title.setGravity(Gravity.CENTER_VERTICAL);
            title.setPadding(dp(10), 0, dp(10), 0);
            title.setBackgroundColor(Color.rgb(31, 35, 46));
            FrameLayout.LayoutParams titleParams = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, dp(32), Gravity.TOP);
            overlay.addView(title, titleParams);

            WebView webView = new WebView(this);
            webView.setBackgroundColor(Color.BLACK);
            WebSettings settings = webView.getSettings();
            settings.setJavaScriptEnabled(true);
            settings.setDomStorageEnabled(true);
            settings.setMediaPlaybackRequiresUserGesture(false);
            settings.setLoadsImagesAutomatically(true);
            settings.setUserAgentString("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
            webView.setWebChromeClient(new android.webkit.WebChromeClient());
            webView.setWebViewClient(new WebViewClient() {
                @Override public void onPageFinished(WebView view, String url) {
                    super.onPageFinished(view, url);
                    if (!scrapingExportChannel || generation != exportVideoGeneration)
                        return;
                    scrapeProjectSekaiVideo(generation, view);
                }
            });
            FrameLayout.LayoutParams videoParams = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT);
            videoParams.topMargin = dp(32);
            overlay.addView(webView, videoParams);
            ((ViewGroup)findViewById(android.R.id.content)).addView(overlay, overlayParams);
            exportVideoOverlay = overlay;
            exportVideoWebView = webView;
            webView.loadUrl(PROJECT_SEKAI_VIDEOS);
        });
    }

    private void scrapeProjectSekaiVideo(int generation, WebView webView) {
        if (generation != exportVideoGeneration || exportVideoWebView != webView)
            return;
        final String script = "(function(){try{" +
                "var cards=document.querySelectorAll('ytd-rich-item-renderer,ytd-grid-video-renderer,ytd-video-renderer');" +
                "var ids=[];" +
                "for(var i=0;i<cards.length;i++){var c=cards[i];" +
                "if(c.querySelector('ytd-thumbnail-overlay-time-status-renderer[overlay-style=\\\"LIVE\\\"],ytd-thumbnail-overlay-time-status-renderer[overlay-style=\\\"UPCOMING\\\"],ytd-badge-supported-renderer[aria-label*=\\\"Live\\\"],ytd-badge-supported-renderer[aria-label*=\\\"ライブ\\\"]'))continue;" +
                "var text=(c.innerText||'').toLowerCase();" +
                "if(/live now|upcoming|premiere|生放送|生配信|配信中|配信予定|ワンダショちゃんねる|プロセカ放送局|公式生放送/.test(text))continue;" +
                "var link=c.querySelector('a#thumbnail[href*=\\\"/watch\\\"]');if(!link)continue;" +
                "var m=link.href.match(/[?&]v=([a-zA-Z0-9_-]{11})/);if(!m)continue;" +
                "var duration=(c.querySelector('ytd-thumbnail-overlay-time-status-renderer')||{}).innerText||'';" +
                "var t=duration.match(/(?:(\\d+):)?(\\d{1,2}):(\\d{2})/);" +
                "if(t){var seconds=(parseInt(t[1]||'0')*3600)+(parseInt(t[2])*60)+parseInt(t[3]);if(seconds>=3600)continue;}" +
                "if(ids.indexOf(m[1])<0)ids.push(m[1]);}" +
                "return ids.join(',');}catch(e){return '';}})();";
        webView.evaluateJavascript(script, response -> {
            if (generation != exportVideoGeneration || exportVideoWebView != webView)
                return;
            String ids = "";
            try {
                Object value = new org.json.JSONTokener(response == null ? "\"\"" : response).nextValue();
                if (value instanceof String)
                    ids = (String)value;
            } catch (Exception ignored) {
            }
            String[] candidates = ids.isEmpty() ? new String[0] : ids.split(",");
            if (candidates.length > 0) {
                String selected = candidates[(int)(Math.random() * candidates.length)];
                if (candidates.length > 1 && selected.equals(lastExportVideoId))
                    selected = candidates[(java.util.Arrays.asList(candidates).indexOf(selected) + 1) % candidates.length];
                scrapingExportChannel = false;
                playExportVideo(webView, generation, selected);
                return;
            }
            if (++exportVideoScrapeAttempts < 25) {
                exportVideoHandler.postDelayed(() -> scrapeProjectSekaiVideo(generation, webView), 1000);
            } else {
                TextView unavailable = new TextView(this);
                unavailable.setText("No regular videos found");
                unavailable.setTextColor(Color.WHITE);
                unavailable.setGravity(Gravity.CENTER);
                exportVideoOverlay.addView(unavailable, new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
            }
        });
    }

    private void playExportVideo(WebView webView, int generation, String videoId) {
        if (generation != exportVideoGeneration || exportVideoWebView != webView)
            return;
        lastExportVideoId = videoId;
        webView.loadUrl("https://www.youtube.com/embed/" + videoId
                + "?autoplay=1&mute=1&playsinline=1&controls=1&rel=0");
    }

    public void hideExportVideoPlayer() {
        runOnUiThread(() -> {
            hideExportVideoPlayerOnUiThread();
            stopService(new Intent(this, VideoExportService.class));
        });
    }

    public void updateExportProgress(long frames, long total, boolean finalizing) {
        VideoExportService.updateProgress(frames, total, finalizing);
    }

    public void openUrl(String url) {
        runOnUiThread(() -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            } catch (Exception error) {
                Log.e("MMW", "Could not open link: " + url, error);
            }
        });
    }

    private void hideExportVideoPlayerOnUiThread() {
        getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        ++exportVideoGeneration;
        scrapingExportChannel = false;
        exportVideoHandler.removeCallbacksAndMessages(null);
        if (exportVideoWebView != null) {
            exportVideoWebView.stopLoading();
            exportVideoWebView.loadUrl("about:blank");
        }
        if (exportVideoOverlay != null) {
            ViewGroup parent = (ViewGroup)exportVideoOverlay.getParent();
            if (parent != null)
                parent.removeView(exportVideoOverlay);
            exportVideoOverlay = null;
        }
        if (exportVideoWebView != null) {
            exportVideoWebView.destroy();
            exportVideoWebView = null;
        }
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    public void setTextInputEnabled(boolean enabled) {
        runOnUiThread(() -> {
            if (imeTarget == null || imeActive == enabled)
                return;
            imeActive = enabled;
            InputMethodManager manager = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
            if (enabled) {
                imeTarget.setVisibility(View.VISIBLE);
                imeTarget.requestFocus();
                imeTarget.postDelayed(() -> manager.showSoftInput(imeTarget, InputMethodManager.SHOW_IMPLICIT), 100);
            } else {
                manager.hideSoftInputFromWindow(imeTarget.getWindowToken(), 0);
                imeTarget.clearFocus();
                imeTarget.setVisibility(View.GONE);
            }
        });
    }

    public void requestDocument(long id, String title, boolean save, String extensions, String suggestedName) {
        runOnUiThread(() -> {
            pickerId = id;
            pickerSave = save;
            pickerExtensions = extensions;
            Intent intent = new Intent(save ? Intent.ACTION_CREATE_DOCUMENT : Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            String initialName = suggestedName == null ? "" : suggestedName.trim();
            String extension = defaultExtension();
            if (extension.startsWith("."))
                extension = extension.substring(1);
            if (save && extension != null && !extension.isEmpty()) {
                if (initialName.isEmpty() || initialName.equalsIgnoreCase("." + extension))
                    initialName = title.toLowerCase(Locale.ROOT).contains("mp4") ? "video" : "chart";
                if (!initialName.toLowerCase(Locale.ROOT).endsWith("." + extension.toLowerCase(Locale.ROOT)))
                    initialName += "." + extension;
            }
            intent.putExtra(Intent.EXTRA_TITLE, initialName.isEmpty() ? title : initialName);
            if (!save) {
                String[] mimeTypes = mimeTypes(extensions);
                if (mimeTypes.length > 0)
                    intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
            }
            startActivityForResult(intent, PICKER_REQUEST);
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICKER_REQUEST)
            return;

        String localPath = "";
        if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try {
                if (pickerSave) {
                    String displayName = displayName(uri);
                    if (displayName == null || displayName.isEmpty())
                        displayName = "chart" + defaultExtension();
                    String extension = defaultExtension();
                    if (extension.startsWith("."))
                        extension = extension.substring(1);
                    if (extension != null && !extension.isEmpty()
                            && !displayName.toLowerCase(Locale.ROOT).endsWith("." + extension.toLowerCase(Locale.ROOT))) {
                        displayName += "." + extension;
                        try {
                            Uri renamed = android.provider.DocumentsContract.renameDocument(getContentResolver(), uri, displayName);
                            if (renamed != null)
                                uri = renamed;
                        } catch (Exception ignored) {
                            // Keep the export destination; the local encoder path still needs the extension.
                        }
                    }
                    File directory = new File(getFilesDir(), "exports");
                    if (!directory.exists())
                        directory.mkdirs();
                    File local = new File(directory, displayName);
                    pendingExports.put(local.getAbsolutePath(), uri);
                    localPath = local.getAbsolutePath();
                } else {
                    String displayName = displayName(uri);
                    if (displayName == null || displayName.isEmpty())
                        displayName = "import" + defaultExtension();
                    File directory = new File(getFilesDir(), "imports");
                    if (!directory.exists())
                        directory.mkdirs();
                    File local = new File(directory, System.currentTimeMillis() + "_" + new File(displayName).getName());
                    try (InputStream input = getContentResolver().openInputStream(uri);
                         OutputStream output = new FileOutputStream(local)) {
                        if (input == null)
                            throw new IllegalStateException("Could not read the selected document");
                        byte[] buffer = new byte[32768];
                        int count;
                        while ((count = input.read(buffer)) >= 0)
                            output.write(buffer, 0, count);
                    }
                    localPath = local.getAbsolutePath();
                    importedDocuments.put(localPath, uri);
                    try {
                        int granted = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                        getContentResolver().takePersistableUriPermission(uri, granted);
                    } catch (SecurityException ignored) {
                    }
                }
            } catch (Exception exception) {
                localPath = "";
            }
        }
        nativePickerResult(pickerId, localPath, pickerSave);
    }

    public boolean commitDocument(String filename) {
        Uri uri = pendingExports.remove(filename);
        boolean isNewExport = uri != null;
        if (uri == null)
            uri = importedDocuments.get(filename);
        if (uri == null)
            return true;
        File local = new File(filename);
        try (InputStream input = new FileInputStream(local);
             OutputStream output = getContentResolver().openOutputStream(uri, "wt")) {
            if (output == null)
                return false;
            byte[] buffer = new byte[32768];
            int count;
            while ((count = input.read(buffer)) >= 0)
                output.write(buffer, 0, count);
            output.flush();
            if (isNewExport)
                local.delete();
            return true;
        } catch (Exception exception) {
            return false;
        }
    }

    public long startFfmpegExport(String[] arguments) {
        try {
            FFmpegSession session = FFmpegKit.executeWithArgumentsAsync(arguments, completed -> {
                Log.i("MMWFFmpeg", "FFmpeg completed with state " + completed.getState()
                        + ", return code " + completed.getReturnCode() + ": " + completed.getOutput());
            }, line -> Log.i("MMWFFmpegLog", line.getMessage()), null);
            if (session == null) {
                Log.e("MMWFFmpeg", "FFmpegKit returned no session");
                return 0;
            }
            long id = session.getSessionId();
            ffmpegExportSessions.put(id, session);
            return id;
        } catch (Throwable error) {
            Log.e("MMWFFmpeg", "Could not start FFmpegKit session", error);
            return 0;
        }
    }

    public int getFfmpegExportStatus(long id) {
        FFmpegSession session = ffmpegExportSessions.get(id);
        if (session == null)
            return -1;
        SessionState state = session.getState();
        if (state == SessionState.CREATED || state == SessionState.RUNNING)
            return Integer.MAX_VALUE;
        ReturnCode result = session.getReturnCode();
        if (result == null || !ReturnCode.isSuccess(result))
            Log.e("MMWFFmpeg", "FFmpeg session " + id + " failed with return code " + result + ": " + session.getOutput());
        ffmpegExportSessions.remove(id);
        return result == null ? -1 : result.getValue();
    }

    public void cancelFfmpegExport(long id) {
        FFmpegKit.cancel(id);
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0)
                    return cursor.getString(index);
            }
        } catch (Exception ignored) {
        }
        return uri.getLastPathSegment();
    }

    private String defaultExtension() {
        if (pickerExtensions == null)
            return ".mmws";
        for (String value : pickerExtensions.split(";")) {
            value = value.trim();
            if (value.startsWith("*.") && value.length() > 2)
                return value.substring(1);
        }
        return ".mmws";
    }

    private static String[] mimeTypes(String extensions) {
        if (extensions == null)
            return new String[0];
        java.util.ArrayList<String> result = new java.util.ArrayList<>();
        for (String pattern : extensions.split(";")) {
            pattern = pattern.trim().toLowerCase();
            if (pattern.startsWith("*.")) {
                String extension = pattern.substring(2);
                String mime;
                switch (extension) {
                    case "png": mime = "image/png"; break;
                    case "jpg":
                    case "jpeg": mime = "image/jpeg"; break;
                    case "mp3": mime = "audio/mpeg"; break;
                    case "wav": mime = "audio/wav"; break;
                    case "ogg": mime = "audio/ogg"; break;
                    case "flac": mime = "audio/flac"; break;
                    case "mp4": mime = "video/mp4"; break;
                    default: mime = null; break;
                }
                if (mime != null && !result.contains(mime))
                    result.add(mime);
            }
        }
        return result.toArray(new String[0]);
    }

    private static native void nativePickerResult(long id, String localPath, boolean save);
    private static native void nativeTextInput(String text);
    private static native void nativeKeyInput(int keyCode, boolean down);
}
