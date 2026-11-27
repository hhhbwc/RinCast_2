package app.rincast.vr;

import android.Manifest;
import android.app.NativeActivity;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.speech.tts.TextToSpeech;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

/**
 * The whole app is native (OpenXR + OpenGL ES in librincast.so). This class only
 * provides the few platform services that have no NDK equivalent: HTTPS,
 * runtime permissions, Wi-Fi performance locks and a stable device id.
 * Native code calls these methods through JNI.
 */
public class MainActivity extends NativeActivity {
    static {
        System.loadLibrary("rincast");
    }

    private WifiManager.WifiLock wifiLock;
    private WifiManager.WifiLock wifiHighPerfLock;
    private WifiManager.MulticastLock multicastLock;
    private PowerManager.WakeLock wakeLock;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.RECORD_AUDIO}, 1);
        }
    }

    @Override
    protected void onDestroy() {
        setStreamingLocks(false);
        setUsbNetwork(false);
        for (int i = 0; i < uiSounds.length; i++) {
            if (uiSounds[i] != null) {
                try { uiSounds[i].release(); } catch (Throwable ignored) {}
                uiSounds[i] = null;
            }
        }
        if (tts != null) {
            try { tts.shutdown(); } catch (Throwable ignored) {}
            tts = null;
            ttsReady = false;
        }
        super.onDestroy();
    }

    // ---- A USB cable to the PC (Horizon OS 2.5+) ------------------------------------
    // The headset offers the cable as a network of its own: IPv6 link-local, no internet.
    // Asked for once; the user allows it in a system dialog, which only shows while RinCast is
    // in front. Native code polls usbNetworkState().

    private ConnectivityManager.NetworkCallback usbCallback;
    private volatile int usbState = 0;  // 0 off, 1 waiting for a cable, 2 connected, 3 not allowed
    private volatile long usbHandle = 0;
    private volatile String usbInterface = "";
    private volatile int usbKbps = 0;

    public synchronized void setUsbNetwork(boolean on) {
        final ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return;
        if (!on) {
            if (usbCallback != null) {
                try { cm.unregisterNetworkCallback(usbCallback); } catch (Throwable ignored) { }
                usbCallback = null;
            }
            usbState = 0;
            usbHandle = 0;
            usbInterface = "";
            return;
        }
        if (usbCallback != null) return;
        try {
            final NetworkRequest.Builder builder = new NetworkRequest.Builder();
            // NetworkCapabilities.TRANSPORT_USB was introduced in Android 12 (API 31).
            // On Android 10/11 (such as PICO OS based on Android 10), TRANSPORT_USB throws IllegalArgumentException: Invalid TransportType 8.
            if (Build.VERSION.SDK_INT >= 31) {
                builder.addTransportType(NetworkCapabilities.TRANSPORT_USB);
            } else {
                builder.addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET);
            }
            builder.removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
            builder.removeCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED);
            final NetworkRequest request = builder.build();
            usbCallback = new ConnectivityManager.NetworkCallback() {
                @Override public void onAvailable(Network network) {
                    usbHandle = network.getNetworkHandle();
                    LinkProperties lp = cm.getLinkProperties(network);
                    if (lp != null && lp.getInterfaceName() != null) usbInterface = lp.getInterfaceName();
                    usbState = 2;
                }
                @Override public void onLinkPropertiesChanged(Network network, LinkProperties lp) {
                    if (lp.getInterfaceName() != null) usbInterface = lp.getInterfaceName();
                }
                @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                    usbKbps = caps.getLinkDownstreamBandwidthKbps();
                }
                @Override public void onLost(Network network) {
                    usbHandle = 0;
                    usbState = 1;
                }
                @Override public void onUnavailable() {
                    // Declined, or asked while in the background: the request is gone.
                    synchronized (MainActivity.this) { usbCallback = null; }
                    usbHandle = 0;
                    usbState = 3;
                }
            };
            usbState = 1;
            cm.requestNetwork(request, usbCallback);
        } catch (Throwable e) {
            usbCallback = null;
            usbHandle = 0;
            usbInterface = "";
            usbState = 3;
        }
    }

    /** "state,handle,kbps,interface". */
    public String usbNetworkState() {
        return usbState + "," + usbHandle + "," + usbKbps + "," + usbInterface;
    }

    /**
     * Dynamically queries hardware decoder capabilities:
     * returns int[] of [h264, hevc, hevc10, av1, av110, maxWidth, maxHeight].
     */
    public int[] queryVideoDecoderCaps() {
        final int[] result = new int[]{1, 1, 0, 0, 0, 4096, 4096};
        try {
            int maxW = 0, maxH = 0;
            boolean anyHw = false;
            boolean hasH264 = false, hasHevc = false, hasHevc10 = false, hasAv1 = false, hasAv110 = false;
            MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
            MediaCodecInfo[] codecInfos = list.getCodecInfos();
            if (codecInfos != null) {
                for (MediaCodecInfo info : codecInfos) {
                    if (info == null || info.isEncoder()) continue;
                    if (Build.VERSION.SDK_INT >= 29) {
                        try {
                            if (info.isSoftwareOnly()) continue;
                        } catch (Throwable ignored) {}
                    }
                    if (info.getName() == null) continue;
                    String name = info.getName().toLowerCase(java.util.Locale.ROOT);
                    if (name.contains(".secure") || name.startsWith("c2.android.") || name.startsWith("omx.google.") ||
                        name.startsWith("omx.ffmpeg.") || name.startsWith("c2.google.")) {
                        continue;
                    }
                    String[] types = info.getSupportedTypes();
                    if (types == null) continue;
                    for (String type : types) {
                        if (type.equalsIgnoreCase("video/avc")) {
                            anyHw = true;
                            hasH264 = true;
                            try {
                                MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                                if (caps != null && caps.getVideoCapabilities() != null) {
                                    maxW = Math.max(maxW, caps.getVideoCapabilities().getSupportedWidths().getUpper());
                                    maxH = Math.max(maxH, caps.getVideoCapabilities().getSupportedHeights().getUpper());
                                }
                            } catch (Throwable ignored) {}
                        } else if (type.equalsIgnoreCase("video/hevc")) {
                            anyHw = true;
                            hasHevc = true;
                            try {
                                MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                                if (caps != null) {
                                    if (caps.profileLevels != null) {
                                        for (MediaCodecInfo.CodecProfileLevel pl : caps.profileLevels) {
                                            if (pl.profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10 ||
                                                pl.profile == 0x1000 || pl.profile == 0x2000) {
                                                hasHevc10 = true;
                                                break;
                                            }
                                        }
                                    }
                                    if (caps.getVideoCapabilities() != null) {
                                        maxW = Math.max(maxW, caps.getVideoCapabilities().getSupportedWidths().getUpper());
                                        maxH = Math.max(maxH, caps.getVideoCapabilities().getSupportedHeights().getUpper());
                                    }
                                }
                            } catch (Throwable ignored) {}
                        } else if (type.equalsIgnoreCase("video/av01")) {
                            anyHw = true;
                            hasAv1 = true;
                            try {
                                MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                                if (caps != null) {
                                    if (caps.profileLevels != null) {
                                        for (MediaCodecInfo.CodecProfileLevel pl : caps.profileLevels) {
                                            if (pl.profile == 0x2 /* AV1ProfileMain10 */ ||
                                                pl.profile == 0x1000 || pl.profile == 0x2000) {
                                                hasAv110 = true;
                                                break;
                                            }
                                        }
                                    }
                                    if (caps.getVideoCapabilities() != null) {
                                        maxW = Math.max(maxW, caps.getVideoCapabilities().getSupportedWidths().getUpper());
                                        maxH = Math.max(maxH, caps.getVideoCapabilities().getSupportedHeights().getUpper());
                                    }
                                }
                            } catch (Throwable ignored) {}
                        }
                    }
                }
            }
            if (anyHw) {
                result[0] = hasH264 ? 1 : 0;
                result[1] = hasHevc ? 1 : 0;
            }
            if (hasHevc10) result[2] = 1;
            if (hasAv1) result[3] = 1;
            if (hasAv110) result[4] = 1;
            if (maxW > 0) result[5] = maxW;
            if (maxH > 0) result[6] = maxH;
        } catch (Throwable ignored) {}
        return result;
    }

    /** Stable random id for this installation (used to recognize the headset). */
    public String getRinCastDeviceId() {
        SharedPreferences prefs = getSharedPreferences("rincast", Context.MODE_PRIVATE);
        String id = prefs.getString("deviceId", null);
        if (id == null) {
            id = UUID.randomUUID().toString().replace("-", "").substring(0, 24);
            prefs.edit().putString("deviceId", id).apply();
        }
        return id;
    }

    public String getDeviceModel() {
        return Build.MODEL;
    }

    public String getFilesPath() {
        return getFilesDir().getAbsolutePath();
    }

    /** The system's language as a BCP 47 tag, e.g. "zh-CN", for the UI's "auto" language. */
    public String getSystemLocale() {
        try {
            android.os.LocaleList locales = android.content.res.Resources.getSystem().getConfiguration().getLocales();
            if (locales != null && !locales.isEmpty() && locales.get(0) != null) {
                return locales.get(0).toLanguageTag();
            }
        } catch (Throwable ignored) {}
        try {
            return java.util.Locale.getDefault().toLanguageTag();
        } catch (Throwable ignored) {}
        return "en-US";
    }

    public boolean hasMicPermission() {
        return checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Keeps Wi-Fi in its low-latency mode and the CPU awake while streaming;
     * the multicast lock lets us receive LAN discovery broadcasts.
     */
    public synchronized void setStreamingLocks(boolean enable) {
        WifiManager wifi = null;
        try {
            wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        } catch (Throwable ignored) {}
        if (enable) {
            if (wifiLock == null && wifi != null) {
                try {
                    int mode = Build.VERSION.SDK_INT >= 29 ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                           : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
                    wifiLock = wifi.createWifiLock(mode, "rincast:stream");
                    wifiLock.acquire();
                } catch (Throwable ignored) { wifiLock = null; }
            }
            if (wifiHighPerfLock == null && wifi != null && Build.VERSION.SDK_INT >= 29) {
                try {
                    wifiHighPerfLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "rincast:stream-hp");
                    wifiHighPerfLock.acquire();
                } catch (Throwable ignored) { wifiHighPerfLock = null; }
            }
            if (multicastLock == null && wifi != null) {
                try {
                    multicastLock = wifi.createMulticastLock("rincast:discovery");
                    multicastLock.acquire();
                } catch (Throwable ignored) { multicastLock = null; }
            }
            if (wakeLock == null) {
                try {
                    PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
                    if (pm != null) {
                        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "rincast:stream");
                        wakeLock.acquire();
                    }
                } catch (Throwable ignored) { wakeLock = null; }
            }
        } else {
            if (wifiLock != null) { try { wifiLock.release(); } catch (Throwable ignored) {} wifiLock = null; }
            if (wifiHighPerfLock != null) { try { wifiHighPerfLock.release(); } catch (Throwable ignored) {} wifiHighPerfLock = null; }
            if (multicastLock != null) { try { multicastLock.release(); } catch (Throwable ignored) {} multicastLock = null; }
            if (wakeLock != null) { try { wakeLock.release(); } catch (Throwable ignored) {} wakeLock = null; }
        }
    }

    // ---- Interface sounds and spoken labels -----------------------------------------

    private final AudioTrack[] uiSounds = new AudioTrack[3];
    private TextToSpeech tts;
    private volatile boolean ttsReady = false;
    private volatile String pendingSpeech;

    // A short soft tone: one note per frequency, each with a fast decay.
    private static AudioTrack makeSound(double[] notes, int noteMs, double volume) {
        final int rate = 48000, noteFrames = rate * noteMs / 1000;
        final short[] pcm = new short[noteFrames * notes.length];
        for (int n = 0; n < notes.length; n++) {
            for (int i = 0; i < noteFrames; i++) {
                final double t = (double) i / rate;
                final double envelope = Math.min(1.0, i / 96.0) * Math.exp(-t * 2000.0 / noteMs / 4.0);
                pcm[n * noteFrames + i] = (short) (Math.sin(2 * Math.PI * notes[n] * t) * envelope * volume * 32767);
            }
        }
        final AudioTrack track = new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(pcm.length * 2)
            .setTransferMode(AudioTrack.MODE_STATIC).build();
        track.write(pcm, 0, pcm.length);
        return track;
    }

    public void playUiSound(int kind) {
        try {
            if (kind < 0 || kind >= uiSounds.length) return;
            if (uiSounds[0] == null) {
                uiSounds[0] = makeSound(new double[] {1400}, 25, 0.12);
                uiSounds[1] = makeSound(new double[] {660, 990}, 90, 0.2);
                uiSounds[2] = makeSound(new double[] {440, 330}, 130, 0.22);
            }
            final AudioTrack t = uiSounds[kind];
            if (t == null) return;
            t.stop();
            t.reloadStaticData();
            t.play();
        } catch (Throwable e) {
            // Sounds are a nicety: never let them disturb the app.
        }
    }

    /** Asks for those of the permissions (separated by commas) that aren't granted yet. */
    public void requestPermissionList(String list) {
        final java.util.ArrayList<String> missing = new java.util.ArrayList<>();
        for (String p : list.split(",")) {
            if (!p.isEmpty() && checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        }
        if (!missing.isEmpty()) runOnUiThread(() -> requestPermissions(missing.toArray(new String[0]), 2));
    }

    public void speak(String text) {
        runOnUiThread(() -> {
            try {
                if (ttsReady && tts != null) {
                    tts.speak(text, TextToSpeech.QUEUE_FLUSH, null, "rincast-ui");
                    return;
                }
                pendingSpeech = text;
                if (tts == null) {
                    tts = new TextToSpeech(MainActivity.this, status -> {
                        ttsReady = status == TextToSpeech.SUCCESS;
                        if (ttsReady && tts != null && pendingSpeech != null) {
                            try { tts.speak(pendingSpeech, TextToSpeech.QUEUE_FLUSH, null, "rincast-ui"); } catch (Throwable ignored) {}
                        }
                    });
                }
            } catch (Throwable ignored) {
                ttsReady = false;
            }
        });
    }

    private static boolean isLocalHost(String host) {
        final String h = host.toLowerCase(java.util.Locale.ROOT);
        if (h.equals("localhost") || h.endsWith(".local") || h.equals("[::1]") || h.startsWith("[fe80:") || h.startsWith("[fd") || h.startsWith("[fc")) return true;
        final String[] p = h.split("\\.");
        if (p.length != 4) return false;
        try {
            final int a = Integer.parseInt(p[0]), b = Integer.parseInt(p[1]);
            return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
        } catch (NumberFormatException e) {
            return false;
        }
    }

    /**
     * Blocking HTTP(S) request, called from a native worker thread.
     * Returns "<status>\n<body>"; status 0 means a network error.
     */
    public String httpRequest(String method, String url, String body, String bearer, int timeoutMs) {
        HttpURLConnection conn = null;
        try {
            // Sign-in tokens never travel unencrypted over the internet. Plain http is only
            // for a self-hosted server on the same network.
            final URL parsed = new URL(url);
            if (parsed.getProtocol().equals("http") && !isLocalHost(parsed.getHost())) {
                throw new java.io.IOException("This server needs an https:// address.");
            }
            conn = (HttpURLConnection) parsed.openConnection();
            conn.setRequestMethod(method);
            conn.setConnectTimeout(timeoutMs);
            conn.setReadTimeout(timeoutMs);
            conn.setRequestProperty("Accept", "application/json");
            conn.setRequestProperty("User-Agent", "RinCast-Headset/1.0");
            if (bearer != null && !bearer.isEmpty()) {
                conn.setRequestProperty("Authorization", "Bearer " + bearer);
            }
            if (body != null && !body.isEmpty()) {
                conn.setDoOutput(true);
                conn.setRequestProperty("Content-Type", "application/json");
                byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
                conn.setFixedLengthStreamingMode(bytes.length);
                try (OutputStream os = conn.getOutputStream()) {
                    os.write(bytes);
                }
            }
            int status = conn.getResponseCode();
            InputStream is = status >= 400 ? conn.getErrorStream() : conn.getInputStream();
            String text = "";
            if (is != null) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int n;
                while ((n = is.read(buf)) > 0) out.write(buf, 0, n);
                is.close();
                text = out.toString("UTF-8");
            }
            return status + "\n" + text;
        } catch (Exception e) {
            return "0\n" + e.getClass().getSimpleName() + ": " + e.getMessage();
        } finally {
            if (conn != null) conn.disconnect();
        }
    }
}
