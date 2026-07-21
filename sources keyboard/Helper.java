import android.app.Activity;
import android.content.Context;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.Toast;

public class Helper {

    private static KeyboardView keyboardView;
    private static Activity boundActivity;
    private static final Handler ui = new Handler(Looper.getMainLooper());

    static Activity activity() {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getMethod("currentActivityThread").invoke(null);
            java.lang.reflect.Field mf = at.getDeclaredField("mActivities");
            mf.setAccessible(true);
            for (Object record : ((java.util.Map<?, ?>) mf.get(thread)).values()) {
                java.lang.reflect.Field af = record.getClass().getDeclaredField("activity");
                af.setAccessible(true);
                Activity a = (Activity) af.get(record);
                if (a != null)
                    return a;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    public static void showToast(String msg) {
        Activity a = activity();
        if (a != null)
            ui.post(() -> Toast.makeText(a, msg, Toast.LENGTH_SHORT).show());
    }

    public static String getPath(String type) {
        return Environment.getExternalStoragePublicDirectory(type).getAbsolutePath();
    }

    public static void showKeyboard(boolean show) {
        Activity activity = activity();
        if (activity == null)
            return;

        ui.post(() -> {
            InputMethodManager imm = (InputMethodManager)
                activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm == null)
                return;

            if (!show) {
                if (keyboardView != null) {
                    imm.hideSoftInputFromWindow(keyboardView.getWindowToken(), 0);
                    keyboardView.clearFocus();
                }
                return;
            }

            if (keyboardView == null || boundActivity != activity) {
                keyboardView = new KeyboardView(activity);
                boundActivity = activity;
                activity.addContentView(keyboardView,
                    new android.view.ViewGroup.LayoutParams(1, 1));
            }

            KeyboardView kv = keyboardView;
            kv.post(() -> {
                kv.requestFocus();
                imm.showSoftInput(kv, InputMethodManager.SHOW_FORCED);
            });
        });
    }

    public static native void nativeSendChar(int unicode);
    public static native void nativeSendKey(int keyCode);

    static class KeyboardView extends View {

        KeyboardView(Context ctx) {
            super(ctx);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo out) {
            out.inputType = android.text.InputType.TYPE_CLASS_TEXT |
                            android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            out.imeOptions = EditorInfo.IME_ACTION_SEND |
                             EditorInfo.IME_FLAG_NO_EXTRACT_UI |
                             EditorInfo.IME_FLAG_NO_FULLSCREEN;

            return new BaseInputConnection(this, false) {

                @Override
                public boolean commitText(CharSequence text, int newCursorPosition) {
                    for (int i = 0; i < text.length(); ) {
                        int cp = Character.codePointAt(text, i);
                        nativeSendChar(cp);
                        i += Character.charCount(cp);
                    }
                    return true;
                }

                @Override
                public boolean deleteSurroundingText(int before, int after) {
                    for (int i = 0; i < before; i++)
                        nativeSendKey(KeyEvent.KEYCODE_DEL);
                    for (int i = 0; i < after; i++)
                        nativeSendKey(KeyEvent.KEYCODE_FORWARD_DEL);
                    return true;
                }

                @Override
                public boolean sendKeyEvent(KeyEvent event) {
                    if (event.getAction() != KeyEvent.ACTION_DOWN)
                        return super.sendKeyEvent(event);

                    int code = event.getKeyCode();
                    if (code == KeyEvent.KEYCODE_DEL || code == KeyEvent.KEYCODE_ENTER ||
                        code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT) {
                        nativeSendKey(code);
                        return true;
                    }

                    int uni = event.getUnicodeChar(event.getMetaState());
                    if (uni != 0) {
                        nativeSendChar(uni);
                        return true;
                    }
                    return super.sendKeyEvent(event);
                }
            };
        }
    }
}
