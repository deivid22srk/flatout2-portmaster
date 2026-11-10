package com.deivid22srk.flatout2;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Environment;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * Verifies the game data and the guest binary, shows clear instructions when
 * something is missing, and starts GameActivity when everything is in place.
 *
 * Expected layout (no storage permission needed, app-specific external dir):
 *   Android/data/com.deivid22srk.flatout2/files/gamedata/{FlatOut2.exe, d3dx9_30.dll,
 *       filesystem, data.bfs, langpack.bfs[, zoom_platform.bfs]}
 *   Android/data/com.deivid22srk.flatout2/files/port/fo2.aarch64
 * (fo2.aarch64 is also extracted from the APK asset fo2/fo2.aarch64 if present.)
 */
public class BootActivity extends Activity {

    static final String REQUIRED_EXE_MD5 = "1f8cae2b47276ca134750ba438aa35ca";

    private TextView status;
    private Button start;
    private Button recheck;
    private LinearLayout problemBox;
    private String report = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(px(24), px(16), px(24), px(16));
        root.setBackgroundColor(Color.parseColor("#101010"));

        TextView title = new TextView(this);
        title.setText(R.string.boot_title);
        title.setTextSize(20);
        title.setTextColor(Color.parseColor("#D24A1E"));
        root.addView(title);

        status = new TextView(this);
        status.setText(R.string.boot_checking);
        status.setTextColor(Color.WHITE);
        root.addView(status);

        problemBox = new LinearLayout(this);
        problemBox.setOrientation(LinearLayout.VERTICAL);
        root.addView(problemBox);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);

        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.HORIZONTAL);
        bottom.setGravity(Gravity.END | Gravity.BOTTOM);

        start = new Button(this);
        start.setText(R.string.boot_start);
        start.setEnabled(false);
        start.setOnClickListener(v -> startActivity(
                new android.content.Intent(this, GameActivity.class)));
        bottom.addView(start);

        recheck = new Button(this);
        recheck.setText(R.string.boot_recheck);
        recheck.setOnClickListener(v -> check());
        bottom.addView(recheck);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        page.addView(bottom, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(page);

        extractBundledBinary();
        check();
    }

    private int px(int dp) {
        return Math.round(dp * getResources().getDisplayMetrics().density);
    }

    private File gamedataDir() {
        return new File(getExternalFilesDir(null), "gamedata");
    }

    private File portDir() {
        return new File(getExternalFilesDir(null), "port");
    }

    static File binaryFile(android.content.Context ctx) {
        return new File(ctx.getExternalFilesDir(null), "port/fo2.aarch64");
    }

    /** Copies the APK-embedded guest binary (CI embeds it under assets/fo2/). */
    private void extractBundledBinary() {
        File out = binaryFile(this);
        try (InputStream in = getAssets().open("fo2/fo2.aarch64")) {
            long assetSize = in.available();
            if (out.exists() && out.length() == assetSize) return; // already there
            portDir().mkdirs();
            try (FileOutputStream fo = new FileOutputStream(out)) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) fo.write(buf, 0, n);
            }
        } catch (IOException e) {
            // No bundled binary: the user must place it manually (documented).
        }
    }

    private void check() {
        StringBuilder problems = new StringBuilder();
        boolean ok = true;

        File gd = gamedataDir();
        File exe = new File(gd, "FlatOut2.exe");
        if (!exe.exists()) exe = new File(gd, "flatout2.exe");

        if (!gd.isDirectory() || !exe.exists()) {
            ok = false;
            problems.append(getString(R.string.boot_missing_title)).append("\n\n")
                    .append(String.format(getString(R.string.boot_missing_body),
                            gamedataDir().getAbsolutePath()))
                    .append("\n\n");
        } else {
            String md5 = md5(exe);
            if (!REQUIRED_EXE_MD5.equalsIgnoreCase(md5)) {
                ok = false;
                problems.append(String.format(getString(R.string.boot_wrong_exe), md5))
                        .append("\n\n");
            }
            for (String name : new String[]{"d3dx9_30.dll", "filesystem", "data.bfs", "langpack.bfs"}) {
                if (!new File(gd, name).exists()) {
                    ok = false;
                    problems.append("Faltando em gamedata/: ").append(name).append("\n");
                }
            }
        }

        File bin = binaryFile(this);
        if (!bin.exists() || bin.length() < 1024 * 1024) {
            ok = false;
            problems.append(String.format(getString(R.string.boot_binary_missing),
                    portDir().getAbsolutePath()));
        }

        problemBox.removeAllViews();
        report = problems.toString();
        if (!report.isEmpty()) {
            TextView tv = new TextView(this);
            tv.setText(report);
            tv.setTextColor(Color.parseColor("#FFB0B0"));
            tv.setTextSize(14);
            problemBox.addView(tv);
        }

        status.setText(ok ? getString(R.string.boot_ok)
                          : getString(R.string.boot_missing_title));
        start.setEnabled(ok);
    }

    static String md5(File f) {
        try {
            MessageDigest md = MessageDigest.getInstance("MD5");
            try (FileInputStream in = new FileInputStream(f)) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            }
            StringBuilder sb = new StringBuilder();
            for (byte b : md.digest()) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (NoSuchAlgorithmException | IOException e) {
            return "erro";
        }
    }
}
