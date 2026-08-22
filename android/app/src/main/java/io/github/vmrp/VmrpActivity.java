package io.github.vmrp;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.text.InputFilter;
import android.text.InputType;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;

public class VmrpActivity extends SDLActivity {
    private static final int REQ_IMPORT_MRP = 1106;
    private static final String PREFS = "vmrp";
    private static final String KEY_ASSETS_VER = "assets_ver";
    private static final String KEY_FIRMWARE = "firmware";
    private static final String KEY_IMEI = "imei";
    private static final String KEY_IMSI = "imsi";
    private static final String KEY_MANUF = "manuf";
    private static final String KEY_MODEL = "model";
    private static final String KEY_WIDTH = "width";
    private static final String KEY_HEIGHT = "height";
    private static final String KEY_SF2 = "sf2";
    private static final String DEF_IMEI = "864086040622841";
    private static final String DEF_IMSI = "460019707327302";
    private static final String DEF_MANUF = "vmrp";
    private static final String DEF_MODEL = "andrd";
    private static final int DEF_WIDTH = 240;
    private static final int DEF_HEIGHT = 320;

    public static final int MR_KEY_PRESS = 0;
    public static final int MR_KEY_RELEASE = 1;
    public static final int MR_KEY_0 = 0;
    public static final int MR_KEY_1 = 1;
    public static final int MR_KEY_2 = 2;
    public static final int MR_KEY_3 = 3;
    public static final int MR_KEY_4 = 4;
    public static final int MR_KEY_5 = 5;
    public static final int MR_KEY_6 = 6;
    public static final int MR_KEY_7 = 7;
    public static final int MR_KEY_8 = 8;
    public static final int MR_KEY_9 = 9;
    public static final int MR_KEY_STAR = 10;
    public static final int MR_KEY_POUND = 11;
    public static final int MR_KEY_UP = 12;
    public static final int MR_KEY_DOWN = 13;
    public static final int MR_KEY_LEFT = 14;
    public static final int MR_KEY_RIGHT = 15;
    public static final int MR_KEY_POWER = 16;
    public static final int MR_KEY_SOFTLEFT = 17;
    public static final int MR_KEY_SOFTRIGHT = 18;
    public static final int MR_KEY_SEND = 19;
    public static final int MR_KEY_SELECT = 20;

    private View keypad;
    private AlertDialog editDialog;

    public static native void nativeSetKeypadHeight(int height);
    public static native void nativeKey(int type, int mrKey);
    public static native void nativeEditDone(String text, boolean ok);

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        extractRuntimeFiles();
        writeConfigFile();
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        attachKeypad();
    }

    @Override
    protected void onDestroy() {
        dismissEditDialog();
        super.onDestroy();
    }

    private void attachKeypad() {
        ViewGroup root = findViewById(android.R.id.content);
        if (root == null) {
            return;
        }
        keypad = LayoutInflater.from(this).inflate(R.layout.keypad, root, false);
        root.addView(keypad);
        bindKey(R.id.key_soft_left, MR_KEY_SOFTLEFT);
        bindKey(R.id.key_soft_right, MR_KEY_SOFTRIGHT);
        bindKey(R.id.key_up, MR_KEY_UP);
        bindKey(R.id.key_down, MR_KEY_DOWN);
        bindKey(R.id.key_left, MR_KEY_LEFT);
        bindKey(R.id.key_right, MR_KEY_RIGHT);
        bindKey(R.id.key_ok, MR_KEY_SELECT);
        bindKey(R.id.key_1, MR_KEY_1);
        bindKey(R.id.key_2, MR_KEY_2);
        bindKey(R.id.key_3, MR_KEY_3);
        bindKey(R.id.key_4, MR_KEY_4);
        bindKey(R.id.key_5, MR_KEY_5);
        bindKey(R.id.key_6, MR_KEY_6);
        bindKey(R.id.key_7, MR_KEY_7);
        bindKey(R.id.key_8, MR_KEY_8);
        bindKey(R.id.key_9, MR_KEY_9);
        bindKey(R.id.key_star, MR_KEY_STAR);
        bindKey(R.id.key_0, MR_KEY_0);
        bindKey(R.id.key_pound, MR_KEY_POUND);
        bindCombo(R.id.key_up_left, MR_KEY_UP, MR_KEY_LEFT);
        bindCombo(R.id.key_up_right, MR_KEY_UP, MR_KEY_RIGHT);
        bindCombo(R.id.key_down_left, MR_KEY_DOWN, MR_KEY_LEFT);
        bindCombo(R.id.key_down_right, MR_KEY_DOWN, MR_KEY_RIGHT);
        keypad.findViewById(R.id.btn_menu).setOnClickListener(v -> showMenu());
        keypad.post(() -> nativeSetKeypadHeight(keypad.getHeight()));
        keypad.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) ->
                nativeSetKeypadHeight(v.getHeight()));
    }

    private void bindKey(int viewId, int mrKey) {
        bindCombo(viewId, mrKey);
    }

    private void bindCombo(int viewId, final int... keys) {
        View view = keypad.findViewById(viewId);
        if (view == null || keys.length == 0) {
            return;
        }
        view.setOnTouchListener((v, event) -> {
            switch (event.getActionMasked()) {
                case android.view.MotionEvent.ACTION_DOWN:
                    v.setPressed(true);
                    for (int key : keys) {
                        nativeKey(MR_KEY_PRESS, key);
                    }
                    return true;
                case android.view.MotionEvent.ACTION_UP:
                case android.view.MotionEvent.ACTION_CANCEL:
                    v.setPressed(false);
                    for (int key : keys) {
                        nativeKey(MR_KEY_RELEASE, key);
                    }
                    if (event.getActionMasked() == android.view.MotionEvent.ACTION_UP) {
                        v.performClick();
                    }
                    return true;
                default:
                    return false;
            }
        });
    }

    private void showMenu() {
        String fw = getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY_FIRMWARE, "full");
        String fwLabel = "mini".equals(fw) ? getString(R.string.firmware_mini) : getString(R.string.firmware_full);
        CharSequence[] items = new CharSequence[]{
                getString(R.string.import_mrp),
                fwLabel,
                getString(R.string.device_settings),
                getString(R.string.key_send),
                getString(R.string.key_power),
                getString(R.string.about)
        };
        new AlertDialog.Builder(this)
                .setTitle(R.string.menu_title)
                .setItems(items, (d, which) -> {
                    if (which == 0) {
                        importMrp();
                    } else if (which == 1) {
                        toggleFirmware();
                    } else if (which == 2) {
                        showDeviceSettings();
                    } else if (which == 3) {
                        nativeKey(MR_KEY_PRESS, MR_KEY_SEND);
                        nativeKey(MR_KEY_RELEASE, MR_KEY_SEND);
                    } else if (which == 4) {
                        nativeKey(MR_KEY_PRESS, MR_KEY_POWER);
                        nativeKey(MR_KEY_RELEASE, MR_KEY_POWER);
                    } else if (which == 5) {
                        showAbout();
                    }
                })
                .show();
    }

    public void showEditDialog(final String title, final String text, final int maxSize) {
        runOnUiThread(() -> {
            dismissEditDialog();
            final EditText input = new EditText(this);
            input.setText(text == null ? "" : text);
            if (maxSize > 0) {
                input.setFilters(new android.text.InputFilter[]{
                        new android.text.InputFilter.LengthFilter(maxSize)
                });
            }
            input.setSelection(input.getText().length());
            editDialog = new AlertDialog.Builder(this)
                    .setTitle(title == null || title.isEmpty() ? getString(R.string.edit_title) : title)
                    .setView(input)
                    .setCancelable(true)
                    .setPositiveButton(android.R.string.ok, (d, w) ->
                            nativeEditDone(input.getText().toString(), true))
                    .setNegativeButton(android.R.string.cancel, (d, w) ->
                            nativeEditDone("", false))
                    .setOnCancelListener(d -> nativeEditDone("", false))
                    .create();
            editDialog.show();
        });
    }

    public void dismissEditDialog() {
        runOnUiThread(() -> {
            if (editDialog != null) {
                editDialog.dismiss();
                editDialog = null;
            }
        });
    }

    private void importMrp() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQ_IMPORT_MRP);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_IMPORT_MRP || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        String name = queryDisplayName(uri);
        if (name == null || name.isEmpty()) {
            name = "imported.mrp";
        }
        if (!name.toLowerCase().endsWith(".mrp")) {
            name = name + ".mrp";
        }
        File destDir = new File(getFilesDir(), "mythroad");
        if (!destDir.exists() && !destDir.mkdirs()) {
            Toast.makeText(this, R.string.import_fail, Toast.LENGTH_SHORT).show();
            return;
        }
        File dest = new File(destDir, name);
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(dest)) {
            if (in == null) {
                throw new IOException("openInputStream returned null");
            }
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            Toast.makeText(this, getString(R.string.import_ok, name), Toast.LENGTH_SHORT).show();
        } catch (IOException e) {
            Toast.makeText(this, R.string.import_fail, Toast.LENGTH_SHORT).show();
        }
    }

    private String queryDisplayName(Uri uri) {
        android.database.Cursor cursor = getContentResolver().query(uri, null, null, null, null);
        if (cursor == null) {
            return null;
        }
        try {
            int idx = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
            if (idx >= 0 && cursor.moveToFirst()) {
                return cursor.getString(idx);
            }
        } finally {
            cursor.close();
        }
        return null;
    }

    private void toggleFirmware() {
        SharedPreferences sp = getSharedPreferences(PREFS, MODE_PRIVATE);
        boolean mini = "mini".equals(sp.getString(KEY_FIRMWARE, "full"));
        String next = mini ? "full" : "mini";
        sp.edit().putString(KEY_FIRMWARE, next).apply();
        writeConfigFile();
        Toast.makeText(this, getString(R.string.firmware_restart, next), Toast.LENGTH_LONG).show();
        restartApp();
    }

    private void showAbout() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.app_name)
                .setMessage(R.string.about_text)
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }

    private void restartApp() {
        Intent intent = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (intent != null) {
            intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
        }
        Runtime.getRuntime().exit(0);
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private void ensureDeviceDefaults() {
        SharedPreferences sp = prefs();
        SharedPreferences.Editor ed = sp.edit();
        boolean changed = false;
        if (!sp.contains(KEY_IMEI)) {
            String androidId = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
            ed.putString(KEY_IMEI, toDigits(androidId, 15, DEF_IMEI));
            changed = true;
        }
        if (!sp.contains(KEY_IMSI)) {
            ed.putString(KEY_IMSI, DEF_IMSI);
            changed = true;
        }
        if (!sp.contains(KEY_MANUF)) {
            ed.putString(KEY_MANUF, DEF_MANUF);
            changed = true;
        }
        if (!sp.contains(KEY_MODEL)) {
            ed.putString(KEY_MODEL, DEF_MODEL);
            changed = true;
        }
        if (!sp.contains(KEY_WIDTH)) {
            ed.putInt(KEY_WIDTH, DEF_WIDTH);
            changed = true;
        }
        if (!sp.contains(KEY_HEIGHT)) {
            ed.putInt(KEY_HEIGHT, DEF_HEIGHT);
            changed = true;
        }
        if (changed) {
            ed.apply();
        }
    }

    private EditText addSettingField(LinearLayout form, String label, String value, int inputType, int maxLen) {
        TextView title = new TextView(this);
        title.setText(label);
        title.setPadding(0, 18, 0, 4);
        form.addView(title);
        EditText field = new EditText(this);
        field.setText(value == null ? "" : value);
        field.setInputType(inputType);
        field.setSingleLine(true);
        if (maxLen > 0) {
            field.setFilters(new InputFilter[]{new InputFilter.LengthFilter(maxLen)});
        }
        form.addView(field);
        return field;
    }

    private void showDeviceSettings() {
        ensureDeviceDefaults();
        SharedPreferences sp = prefs();
        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        form.setPadding(pad, pad / 2, pad, pad / 2);
        final EditText imei = addSettingField(form, getString(R.string.label_imei),
                sp.getString(KEY_IMEI, DEF_IMEI), InputType.TYPE_CLASS_NUMBER, 15);
        final EditText imsi = addSettingField(form, getString(R.string.label_imsi),
                sp.getString(KEY_IMSI, DEF_IMSI), InputType.TYPE_CLASS_NUMBER, 15);
        final EditText manuf = addSettingField(form, getString(R.string.label_manuf),
                sp.getString(KEY_MANUF, DEF_MANUF), InputType.TYPE_CLASS_TEXT, 7);
        final EditText model = addSettingField(form, getString(R.string.label_model),
                sp.getString(KEY_MODEL, DEF_MODEL), InputType.TYPE_CLASS_TEXT, 7);
        final EditText width = addSettingField(form, getString(R.string.label_width),
                Integer.toString(sp.getInt(KEY_WIDTH, DEF_WIDTH)), InputType.TYPE_CLASS_NUMBER, 3);
        final EditText height = addSettingField(form, getString(R.string.label_height),
                Integer.toString(sp.getInt(KEY_HEIGHT, DEF_HEIGHT)), InputType.TYPE_CLASS_NUMBER, 3);
        final EditText sf2 = addSettingField(form, getString(R.string.label_sf2),
                sp.getString(KEY_SF2, ""), InputType.TYPE_CLASS_TEXT, 512);
        sf2.setHint(R.string.sf2_hint);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(form);
        new AlertDialog.Builder(this)
                .setTitle(R.string.device_settings)
                .setView(scroll)
                .setPositiveButton(android.R.string.ok, (d, w) -> {
                    int nw;
                    int nh;
                    try {
                        nw = Integer.parseInt(width.getText().toString().trim());
                        nh = Integer.parseInt(height.getText().toString().trim());
                    } catch (NumberFormatException e) {
                        Toast.makeText(this, R.string.device_invalid, Toast.LENGTH_SHORT).show();
                        return;
                    }
                    if (nw < 50 || nw > 800 || nh < 50 || nh > 800) {
                        Toast.makeText(this, R.string.device_invalid, Toast.LENGTH_SHORT).show();
                        return;
                    }
                    prefs().edit()
                            .putString(KEY_IMEI, imei.getText().toString().trim())
                            .putString(KEY_IMSI, imsi.getText().toString().trim())
                            .putString(KEY_MANUF, manuf.getText().toString().trim())
                            .putString(KEY_MODEL, model.getText().toString().trim())
                            .putInt(KEY_WIDTH, nw)
                            .putInt(KEY_HEIGHT, nh)
                            .putString(KEY_SF2, sf2.getText().toString().trim())
                            .apply();
                    writeConfigFile();
                    Toast.makeText(this, R.string.device_saved, Toast.LENGTH_SHORT).show();
                    restartApp();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void writeConfigFile() {
        ensureDeviceDefaults();
        SharedPreferences sp = prefs();
        File cfg = new File(getFilesDir(), "vmrp.cfg");
        try (OutputStreamWriter w = new OutputStreamWriter(new FileOutputStream(cfg), StandardCharsets.UTF_8)) {
            w.write("firmware=" + sp.getString(KEY_FIRMWARE, "full") + "\n");
            w.write("imei=" + sp.getString(KEY_IMEI, DEF_IMEI) + "\n");
            w.write("imsi=" + sp.getString(KEY_IMSI, DEF_IMSI) + "\n");
            w.write("manuf=" + sp.getString(KEY_MANUF, DEF_MANUF) + "\n");
            w.write("model=" + sp.getString(KEY_MODEL, DEF_MODEL) + "\n");
            w.write("width=" + sp.getInt(KEY_WIDTH, DEF_WIDTH) + "\n");
            w.write("height=" + sp.getInt(KEY_HEIGHT, DEF_HEIGHT) + "\n");
            w.write("sf2=" + sp.getString(KEY_SF2, "") + "\n");
        } catch (IOException ignored) {
        }
    }

    private static String toDigits(String src, int len, String fallback) {
        StringBuilder sb = new StringBuilder();
        if (src != null) {
            for (int i = 0; i < src.length() && sb.length() < len; i++) {
                char c = src.charAt(i);
                if (c >= '0' && c <= '9') {
                    sb.append(c);
                } else if (c >= 'a' && c <= 'f') {
                    sb.append((char) ('0' + (c - 'a') % 10));
                } else if (c >= 'A' && c <= 'F') {
                    sb.append((char) ('0' + (c - 'A') % 10));
                }
            }
        }
        if (sb.length() == 0) {
            return fallback;
        }
        while (sb.length() < len) {
            sb.append('0');
        }
        return sb.substring(0, len);
    }

    private void extractRuntimeFiles() {
        int versionCode;
        try {
            versionCode = getPackageManager().getPackageInfo(getPackageName(), 0).versionCode;
        } catch (Exception e) {
            versionCode = 0;
        }
        SharedPreferences sp = getSharedPreferences(PREFS, MODE_PRIVATE);
        int old = sp.getInt(KEY_ASSETS_VER, -1);
        boolean first = old < 0;
        File destRoot = getFilesDir();
        try {
            copyAssetTree(getAssets(), "vmrp", destRoot, first);
            new File(destRoot, "mythroad/disk/a").mkdirs();
            new File(destRoot, "mythroad/disk/b").mkdirs();
            new File(destRoot, "mythroad/disk/x").mkdirs();
            sp.edit().putInt(KEY_ASSETS_VER, versionCode).apply();
        } catch (IOException e) {
            Toast.makeText(this, R.string.extract_fail, Toast.LENGTH_LONG).show();
        }
    }

    private void copyAssetTree(AssetManager am, String assetDir, File destRoot, boolean firstInstall)
            throws IOException {
        String[] children = am.list(assetDir);
        if (children == null) {
            return;
        }
        if (children.length == 0) {
            String rel = assetDir.startsWith("vmrp/") ? assetDir.substring("vmrp/".length()) : "";
            if (rel.isEmpty()) {
                return;
            }
            File dest = new File(destRoot, rel);
            if (!firstInstall && dest.exists() && !isCorePath(rel)) {
                return;
            }
            dest.getParentFile().mkdirs();
            try (InputStream in = am.open(assetDir);
                 OutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
            return;
        }
        for (String child : children) {
            copyAssetTree(am, assetDir + "/" + child, destRoot, firstInstall);
        }
    }

    private static boolean isCorePath(String rel) {
        return "cfunction.ext".equals(rel)
                || "cfunction_mini.ext".equals(rel)
                || rel.startsWith("mythroad/system/")
                || rel.startsWith("mythroad/plugins/")
                || "mythroad/dsm_gm.mrp".equals(rel)
                || "mythroad/cookie.mrp".equals(rel)
                || "mythroad/mpc.mrp".equals(rel)
                || "mythroad/ydqtwo.mrp".equals(rel);
    }
}
