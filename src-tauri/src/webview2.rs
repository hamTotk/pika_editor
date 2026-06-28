//! WebView2 Runtime の不在検出と最小ネイティブ導入案内（design doc 18章・要件2.3 改訂）。
//!
//! 全Web化により WebView2 が無いとアプリシェルすら描けない。よって Tauri 起動「前」に
//! Runtime の存在をレジストリで確認し、不在/破損なら Win32 MessageBox（WebView 非依存）で
//! 導入を案内して終了する。これが「不在環境を模擬した手動確認」（系統C）の実装経路になる。
//!
//! 検出するレジストリキー（Microsoft 公式の Evergreen Runtime 検出方法）:
//! - HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{GUID}\pv（per-machine / 64bit OS）
//! - HKCU\SOFTWARE\Microsoft\EdgeUpdate\Clients\{GUID}\pv（per-user）
//!
//! GUID = {F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}（Evergreen WebView2 Runtime）。

/// WebView2 Runtime が利用可能かを確認する。
///
/// 利用可能なら `Ok(())`、不在/破損なら案内文言を `Err(String)` で返す。
/// 非 Windows ではビルド対象外だが、テスト/将来のため常に `Ok` を返す。
pub fn ensure_runtime_available() -> Result<(), String> {
    #[cfg(windows)]
    {
        if windows_impl::webview2_runtime_present() {
            Ok(())
        } else {
            Err(MISSING_MESSAGE.to_string())
        }
    }
    #[cfg(not(windows))]
    {
        Ok(())
    }
}

/// 不在時の最小ネイティブダイアログ（WebView 非依存）を表示する。
pub fn show_missing_runtime_dialog(message: &str) {
    #[cfg(windows)]
    {
        windows_impl::message_box(message, "pika — WebView2 ランタイムが必要です");
    }
    #[cfg(not(windows))]
    {
        eprintln!("{message}");
    }
}

/// 不在時のユーザー向け案内文言。Runtime 導入手順へ誘導する。
const MISSING_MESSAGE: &str = "pika の起動には Microsoft Edge WebView2 ランタイムが必要です。\n\n\
     お使いの環境にランタイムが見つかりませんでした。\n\
     下記から Evergreen ランタイムを導入してから再度 pika を起動してください。\n\n\
     https://developer.microsoft.com/microsoft-edge/webview2/";

#[cfg(windows)]
mod windows_impl {
    use crate::util::to_wide;
    use windows_sys::Win32::Foundation::ERROR_SUCCESS;
    use windows_sys::Win32::System::Registry::{
        RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE,
        KEY_READ, KEY_WOW64_64KEY,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        MessageBoxW, MB_ICONERROR, MB_OK, MB_SETFOREGROUND,
    };

    // Evergreen WebView2 Runtime の固定 GUID。
    const RUNTIME_KEY_HKLM: &str =
        r"SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    const RUNTIME_KEY_HKCU: &str =
        r"SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    const VERSION_VALUE: &str = "pv";

    /// WebView2 Runtime が per-machine もしくは per-user で導入済みかを返す。
    pub fn webview2_runtime_present() -> bool {
        // per-machine（64bit ビューを明示）または per-user のどちらかに版（pv）があれば導入済み。
        registry_value_nonempty(HKEY_LOCAL_MACHINE, RUNTIME_KEY_HKLM, KEY_WOW64_64KEY)
            || registry_value_nonempty(HKEY_CURRENT_USER, RUNTIME_KEY_HKCU, 0)
    }

    /// 指定キーの `pv` 値が存在し非空（"0.0.0.0" でない）かを判定する。
    fn registry_value_nonempty(root: HKEY, subkey: &str, extra_sam: u32) -> bool {
        let subkey_w = to_wide(subkey);
        let mut hkey: HKEY = std::ptr::null_mut();
        // SAFETY: 出力 hkey は成功時のみ有効。失敗時は触らない。
        let opened =
            unsafe { RegOpenKeyExW(root, subkey_w.as_ptr(), 0, KEY_READ | extra_sam, &mut hkey) };
        if opened != ERROR_SUCCESS {
            return false;
        }

        let value_w = to_wide(VERSION_VALUE);
        let mut buf = [0u16; 64];
        let mut len: u32 = (buf.len() * std::mem::size_of::<u16>()) as u32;
        // SAFETY: hkey は有効。buf/len はバイト長で渡す。
        let queried = unsafe {
            RegQueryValueExW(
                hkey,
                value_w.as_ptr(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                buf.as_mut_ptr() as *mut u8,
                &mut len,
            )
        };
        // SAFETY: 開いたキーは必ず閉じる。
        unsafe {
            RegCloseKey(hkey);
        }
        if queried != ERROR_SUCCESS {
            return false;
        }

        // len はバイト長。u16 個数へ。終端 NUL を除いた版文字列を取り出す。
        let count = (len as usize) / std::mem::size_of::<u16>();
        let slice = &buf[..count.min(buf.len())];
        let version = String::from_utf16_lossy(slice)
            .trim_end_matches('\0')
            .to_string();
        !version.is_empty() && version != "0.0.0.0"
    }

    /// 最小ネイティブ MessageBox（WebView 非依存）。
    pub fn message_box(text: &str, caption: &str) {
        let text_w = to_wide(text);
        let caption_w = to_wide(caption);
        // SAFETY: いずれも NUL 終端の有効ポインタ。親ウィンドウなし（null）。
        unsafe {
            MessageBoxW(
                std::ptr::null_mut(),
                text_w.as_ptr(),
                caption_w.as_ptr(),
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND,
            );
        }
    }
}
