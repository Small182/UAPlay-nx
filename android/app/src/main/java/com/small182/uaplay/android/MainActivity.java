package com.small182.uaplay.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.UiModeManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.VideoView;
import android.widget.Toast;
import java.util.Locale;

public class MainActivity extends Activity {
    private static final String DEMO_URL = "https://storage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4";
    private static final String PREFS = "uaplay_android";
    private static final String MODE = "display_mode";
    private static final int BG = Color.rgb(11,14,20);
    private static final int SURFACE = Color.rgb(28,36,48);
    private static final int ACCENT = Color.rgb(36,183,170);
    private static final int WHITE = Color.rgb(245,248,252);
    private static final int MUTED = Color.rgb(170,189,203);
    private static final int CHOOSE=0, HOME=1, SETTINGS=2, VIDEO=3;

    private SharedPreferences prefs;
    private int page=CHOOSE;
    private VideoView video;
    private LinearLayout controls;
    private Button playButton;
    private Button firstButton;
    private Runnable hideControls;

    private boolean uk() { return Locale.getDefault().getLanguage().equals("uk"); }
    private String s(String ua,String en) { return uk()?ua:en; }
    private boolean tv() { return prefs!=null && "tv".equals(prefs.getString(MODE,"phone")); }
    private int dp(float n) { return (int)(getResources().getDisplayMetrics().density*n+0.5f); }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        prefs=getSharedPreferences(PREFS,MODE_PRIVATE);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        if(!prefs.contains(MODE)) choose(); else { orientation(); home(); }
    }
    private void orientation() {
        setRequestedOrientation(tv()?ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:
                ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
    }
    @Override public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        if(page==CHOOSE) choose();
        else if(page==HOME) home();
        else if(page==SETTINGS) settings();
    }

    private GradientDrawable bg(int color,int stroke) {
        GradientDrawable d=new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(15));
        if(stroke!=0)d.setStroke(dp(2),stroke);
        return d;
    }
    private TextView text(String str,int size,int color) {
        TextView t=new TextView(this);
        t.setText(str);
        t.setTextColor(color);
        t.setTextSize(size);
        t.setLineSpacing(dp(3),1.06f);
        return t;
    }
    private Button button(String title,Runnable action) {
        Button b=new Button(this);
        b.setText(title);
        b.setTextSize(tv()?22:17);
        b.setTextColor(WHITE);
        b.setAllCaps(false);
        b.setPadding(dp(16),dp(tv()?18:14),dp(16),dp(tv()?18:14));
        b.setBackground(bg(SURFACE,0));
        b.setFocusable(true);
        b.setOnFocusChangeListener((v,focus)->{
            b.setBackground(focus?bg(ACCENT,WHITE):bg(SURFACE,0));
            b.setTextColor(focus?BG:WHITE);
        });
        b.setOnClickListener(v->action.run());
        return b;
    }
    private void addButton(LinearLayout l,Button b) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);
        p.bottomMargin=dp(tv()?15:10);
        l.addView(b,p);
        if(firstButton==null)firstButton=b;
    }
    private void gap(LinearLayout l,int amount) {
        l.addView(new View(this),new LinearLayout.LayoutParams(1,dp(amount)));
    }
    private LinearLayout screen() {
        firstButton=null;
        ScrollView outer=new ScrollView(this);
        outer.setFillViewport(true);
        outer.setBackgroundColor(BG);
        LinearLayout content=new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(tv()?64:22),dp(tv()?36:25),dp(tv()?64:22),dp(30));
        outer.addView(content,new ScrollView.LayoutParams(-1,-2));
        setContentView(outer);
        return content;
    }
    private void focus() { if(firstButton!=null && (tv() || hardwareTv()))firstButton.requestFocus(); }
    private boolean hardwareTv() {
        UiModeManager m=(UiModeManager)getSystemService(UI_MODE_SERVICE);
        return m!=null && m.getCurrentModeType()==Configuration.UI_MODE_TYPE_TELEVISION;
    }
    private void saveMode(String selection,boolean first) {
        prefs.edit().putString(MODE,selection).apply();
        orientation();
        Toast.makeText(this,s("Режим збережено","Mode saved"),Toast.LENGTH_SHORT).show();
        if(first)home();else settings();
    }
    private void choose() {
        page=CHOOSE;
        LinearLayout l=screen();
        l.addView(text(s("ПЕРШИЙ ЗАПУСК","FIRST START"),tv()?18:14,ACCENT));
        gap(l,18);
        l.addView(text(s("Як користуватиметесь UAPlay?","How will you use UAPlay?"),tv()?37:28,WHITE));
        gap(l,12);
        l.addView(text(s("Виберіть режим. Змінити його можна в налаштуваннях.",
                "Choose a mode. You can change it in Settings."),tv()?22:16,MUTED));
        gap(l,30);
        Button phone=button(s("Телефон / планшет\nСенсорне керування","Phone / tablet\nTouch input"),()->saveMode("phone",true));
        Button television=button(s("Телевізор\nКерування пультом","Television\nRemote control"),()->saveMode("tv",true));
        addButton(l,phone);
        addButton(l,television);
        if(hardwareTv())television.requestFocus();else phone.requestFocus();
    }
    private void home() {
        page=HOME;
        LinearLayout l=screen();
        TextView title=text("UAPlay",tv()?49:36,WHITE);
        title.setTypeface(null,1);
        l.addView(title);
        l.addView(text(s("Android · тестова збірка","Android · test build"),tv()?20:14,MUTED));
        gap(l,30);
        l.addView(text(s("Перевірка роботи пульта та плеєра",
                "Remote and player test"),tv()?29:24,ACCENT));
        gap(l,12);
        l.addView(text(s("Тестове відео відкривається без введення адреси. Потрібен інтернет.",
                "Sample video opens without entering a URL. Internet required."),tv()?20:15,MUTED));
        gap(l,22);
        addButton(l,button(s("▶ Тестове відео (Big Buck Bunny)","▶ Sample video (Big Buck Bunny)"),
                ()->watch(DEMO_URL)));
        addButton(l,button(s("Відкрити своє відеопосилання","Open custom video URL"),this::promptUrl));
        addButton(l,button(s("Налаштування","Settings"),this::settings));
        gap(l,12);
        l.addView(text(s("Каталог UAPlay та джерела фільмів ще не перенесені в Android.",
                "UAPlay catalog and movie providers are not yet ported to Android."),tv()?17:13,MUTED));
        focus();
    }
    private void settings() {
        page=SETTINGS;
        LinearLayout l=screen();
        addButton(l,button(s("← Назад","← Back"),this::home));
        gap(l,13);
        l.addView(text(s("Налаштування","Settings"),tv()?36:28,WHITE));
        gap(l,20);
        l.addView(text(s("Режим пристрою","Device mode"),tv()?24:19,ACCENT));
        gap(l,16);
        addButton(l,button(s("Телефон / планшет","Phone / tablet")+(!tv()?" ✓":""),()->saveMode("phone",false)));
        addButton(l,button(s("Телевізор","Television")+(tv()?" ✓":""),()->saveMode("tv",false)));
        gap(l,12);
        l.addView(text(s("Вибір зберігається після перезапуску.",
                "Your choice is saved between launches."),tv()?19:15,MUTED));
        focus();
    }
    private void promptUrl() {
        EditText edit=new EditText(this);
        edit.setSingleLine(true);
        edit.setHint("https://example.com/video.mp4");
        edit.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_VARIATION_URI);
        edit.setText(prefs.getString("last_url",""));
        FrameLayout wrapper=new FrameLayout(this);
        wrapper.setPadding(dp(18),0,dp(18),0);
        wrapper.addView(edit);
        AlertDialog dialog=new AlertDialog.Builder(this)
                .setTitle(s("Пряме HTTPS-посилання MP4/HLS","Direct HTTPS MP4/HLS URL"))
                .setView(wrapper)
                .setNegativeButton(s("Скасувати","Cancel"),(d,w)->{})
                .setPositiveButton(s("Відкрити","Open"),null)
                .create();
        dialog.setOnShowListener(z->dialog.getButton(-1).setOnClickListener(v->{
            String url=edit.getText().toString().trim();
            Uri uri=Uri.parse(url);
            if(!"https".equalsIgnoreCase(uri.getScheme())||uri.getHost()==null) {
                edit.setError(s("Потрібна коректна HTTPS-адреса","A valid HTTPS URL is required"));
                return;
            }
            prefs.edit().putString("last_url",url).apply();
            dialog.dismiss();
            watch(url);
        }));
        dialog.show();
    }
    private void watch(String url) {
        page=VIDEO;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root=new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        video=new VideoView(this);
        root.addView(video,new FrameLayout.LayoutParams(-1,-1));
        controls=new LinearLayout(this);
        controls.setOrientation(LinearLayout.VERTICAL);
        controls.setGravity(Gravity.CENTER);
        controls.setBackgroundColor(Color.argb(220,11,14,20));
        controls.setPadding(dp(15),dp(9),dp(15),dp(12));
        root.addView(controls,new FrameLayout.LayoutParams(-1,-2,Gravity.BOTTOM));
        controls.addView(text(s("Пульт: ← → перемотка · OK пауза · Назад вихід",
                "Remote: ← → seek · OK pause · Back exit"),tv()?17:13,MUTED));
        LinearLayout actions=new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        controls.addView(actions);
        playerButton(actions,button("−10 c",()->seek(-10000)));
        playButton=button("Ⅱ",this::toggle);
        playerButton(actions,playButton);
        playerButton(actions,button("+10 c",()->seek(10000)));
        playerButton(actions,button(s("Назад","Back"),this::leaveVideo));
        setContentView(root);
        video.setOnPreparedListener(mp->{if(video!=null){video.start();showControls();}});
        video.setOnCompletionListener(mp->{if(playButton!=null)playButton.setText("▶");showControls();});
        video.setOnErrorListener((mp,what,extra)->{
            if(!isFinishing())new AlertDialog.Builder(this)
                    .setMessage(s("Не вдалося відтворити відео на цьому пристрої.",
                            "This device could not play the video."))
                    .setPositiveButton("OK",(d,w)->leaveVideo()).show();
            return true;
        });
        video.setOnTouchListener((v,e)->{
            if(e.getAction()==MotionEvent.ACTION_UP) {
                if(controls.getVisibility()==View.VISIBLE)controls.setVisibility(View.GONE);
                else showControls();
            }
            return true;
        });
        video.setVideoURI(Uri.parse(url));
        showControls();
        playButton.requestFocus();
    }
    private void playerButton(LinearLayout row,Button b) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,-2,1);
        p.setMargins(dp(3),dp(12),dp(3),0);
        row.addView(b,p);
    }
    private void toggle() {
        if(video==null)return;
        if(video.isPlaying()){video.pause();playButton.setText("▶");}
        else {video.start();playButton.setText("Ⅱ");}
        showControls();
    }
    private void seek(int delta) {
        if(video==null)return;
        int duration=video.getDuration();
        long next=Math.max(0,(long)video.getCurrentPosition()+delta);
        if(duration>0)next=Math.min(next,duration);
        video.seekTo((int)next);
        showControls();
    }
    private void showControls() {
        if(controls==null)return;
        controls.setVisibility(View.VISIBLE);
        if(hideControls!=null)controls.removeCallbacks(hideControls);
        hideControls=()->{
            if(page==VIDEO && controls!=null && video!=null && video.isPlaying())
                controls.setVisibility(View.GONE);
        };
        controls.postDelayed(hideControls,5500);
    }
    private void leaveVideo() {
        if(controls!=null&&hideControls!=null)controls.removeCallbacks(hideControls);
        if(video!=null){video.stopPlayback();video=null;}
        controls=null;
        playButton=null;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        home();
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if(page==VIDEO && video!=null && event.getAction()==KeyEvent.ACTION_DOWN) {
            switch(event.getKeyCode()) {
                case KeyEvent.KEYCODE_DPAD_LEFT:
                case KeyEvent.KEYCODE_MEDIA_REWIND: seek(-10000);return true;
                case KeyEvent.KEYCODE_DPAD_RIGHT:
                case KeyEvent.KEYCODE_MEDIA_FAST_FORWARD: seek(10000);return true;
                case KeyEvent.KEYCODE_DPAD_CENTER:
                case KeyEvent.KEYCODE_ENTER:
                case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
                case KeyEvent.KEYCODE_MEDIA_PLAY:
                case KeyEvent.KEYCODE_MEDIA_PAUSE: toggle();return true;
                case KeyEvent.KEYCODE_BACK: leaveVideo();return true;
                case KeyEvent.KEYCODE_DPAD_UP:
                case KeyEvent.KEYCODE_DPAD_DOWN: showControls();return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public void onBackPressed() {
        if(page==VIDEO)leaveVideo();
        else if(page==SETTINGS)home();
        else super.onBackPressed();
    }
    @Override protected void onPause() {
        if(video!=null && video.isPlaying()){video.pause();if(playButton!=null)playButton.setText("▶");}
        super.onPause();
    }
    @Override protected void onDestroy() {
        if(controls!=null&&hideControls!=null)controls.removeCallbacks(hideControls);
        if(video!=null)video.stopPlayback();
        super.onDestroy();
    }
}
