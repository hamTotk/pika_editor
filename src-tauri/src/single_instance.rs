//! 単一インスタンスの自前 named pipe 配線（要件3.2/3.4・design doc 9章/15章-9）。
//!
//! 役割（design doc 3章「薄い橋渡し層」）:
//! - 純粋ロジック（パイプ名組立・転送 JSON 組立/スキーマ検証・受信パス再検証・役割決定）は
//!   **すべて pika-core::ipc / pika-core::path_verify**（UI 非依存・cargo test 済み）に委ねる。
//! - ここには OS 呼び出し（`CreateNamedPipe`/`CreateFile`/DACL/`ConvertSidToStringSid`）と
//!   サーバーリスナースレッド・`emit('open-request')` のみを置く。
//!
//! 信頼境界（design doc 15章-9）:
//! - パイプ名は `\\.\pipe\pika-<ユーザーSID>`（ユーザーごとに分離）。
//! - DACL はユーザー限定（`CreateNamedPipe` の lpSecurityAttributes に owner-only DACL）。
//! - `PIPE_REJECT_REMOTE_CLIENTS` でリモート接続を拒否。
//! - 受信は ≤ MAX_MESSAGE_BYTES で打切り、`pika_core::ipc::parse_incoming_message` が
//!   JSON スキーマ検証・version 検証・受信パス再検証（受理操作=パスオープン限定）を行う。
//!
//! 非 Windows ではビルド対象外（named pipe は Windows 固有）。テスト/将来のため空実装を置く。

use pika_core::ipc::{InstanceRole, OpenRequest};
use serde::Serialize;
use tauri::AppHandle;

/// フロントへ送る「このパスを開け」イベントのペイロード（受理操作=パスオープン限定）。
#[derive(Debug, Clone, Serialize)]
pub struct OpenRequestPayload {
    /// 再検証済みの絶対パス群（pika-core が正規化・健全性検査済み）。
    pub paths: Vec<String>,
    /// `-g` カーソル位置（任意）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub goto: Option<GotoDto>,
}

/// `-g` カーソル位置の DTO。
#[derive(Debug, Clone, Serialize)]
pub struct GotoDto {
    pub line: u32,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub column: Option<u32>,
}

impl From<OpenRequest> for OpenRequestPayload {
    fn from(req: OpenRequest) -> Self {
        OpenRequestPayload {
            paths: req.paths,
            goto: req.goto.map(|g| GotoDto {
                line: g.line,
                column: g.column,
            }),
        }
    }
}

/// 起動初期に単一インスタンスのロックを取得し役割を決める。
///
/// サーバー（最初のインスタンス）なら `InstanceRole::Server` を返しリスナースレッドを起動する。
/// クライアント（既に起動済み）なら引数を転送し `InstanceRole::Client` を返す（呼び出し側が
/// 終了コード0で即終了する）。CreateNamedPipe 不能環境（非 Windows）では Server とみなす（縮退）。
pub fn acquire_or_forward(app: &AppHandle, forward_args: &[String]) -> InstanceRole {
    #[cfg(windows)]
    {
        windows_impl::acquire_or_forward(app, forward_args)
    }
    #[cfg(not(windows))]
    {
        let _ = (app, forward_args);
        // named pipe を使えない環境では単一インスタンス制約を諦めサーバー扱い（アプリは起動する）。
        InstanceRole::Server
    }
}

#[cfg(windows)]
mod windows_impl {
    use super::OpenRequestPayload;
    use pika_core::ipc::{
        build_forward_message, build_pipe_name, decide_role, parse_incoming_message, InstanceRole,
        MAX_MESSAGE_BYTES,
    };
    use std::os::windows::ffi::OsStrExt;
    use std::ptr;
    use tauri::{AppHandle, Emitter, Manager};
    use windows_sys::Win32::Foundation::{
        CloseHandle, GetLastError, LocalFree, ERROR_PIPE_BUSY, GENERIC_WRITE, HANDLE,
        INVALID_HANDLE_VALUE,
    };
    use windows_sys::Win32::Security::Authorization::{
        ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
        SDDL_REVISION_1,
    };
    use windows_sys::Win32::Security::{
        GetTokenInformation, TokenUser, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER,
    };
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, ReadFile, WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, OPEN_EXISTING,
        PIPE_ACCESS_INBOUND,
    };
    use windows_sys::Win32::System::Pipes::{
        ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, SetNamedPipeHandleState,
        PIPE_READMODE_MESSAGE, PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_MESSAGE, PIPE_WAIT,
    };
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

    /// パイプの 1 メッセージ受信バッファ上限（core の打切り上限に合わせる）。
    const READ_BUF: usize = MAX_MESSAGE_BYTES;

    /// HANDLE（`*mut c_void`）は `Send` でないため、スレッド移送時は `isize` に詰めて運ぶ。
    /// このハンドルは spawn 後リスナースレッドが単独で所有・操作し共有しない（排他所有の移譲）。
    fn handle_to_isize(h: HANDLE) -> isize {
        h as isize
    }
    fn isize_to_handle(v: isize) -> HANDLE {
        v as HANDLE
    }

    /// SDDL: このユーザー（Owner）とローカル System のみアクセス許可（リモート/他ユーザーを排除）。
    /// `D:` = DACL、`(A;;GA;;;OW)` = Owner に全許可、`(A;;GA;;;SY)` = LocalSystem に全許可。
    const PIPE_SDDL: &str = "D:(A;;GA;;;OW)(A;;GA;;;SY)";

    pub fn acquire_or_forward(app: &AppHandle, forward_args: &[String]) -> InstanceRole {
        // ユーザー SID を取得しパイプ名を組み立てる（core が文字列組立・注入検査）。
        let sid = match current_user_sid() {
            Some(s) => s,
            None => {
                // SID を取れない異常時は単一インスタンスを諦めサーバー扱い（起動はする）。
                return InstanceRole::Server;
            }
        };
        let pipe_name = match build_pipe_name(&sid) {
            Ok(n) => n,
            Err(_) => return InstanceRole::Server,
        };

        // CreateNamedPipe（FIRST_PIPE_INSTANCE）の成否を原子的ロックとする（design doc 9章）。
        match create_server_pipe(&pipe_name) {
            Some(handle) => {
                // ロック獲得＝サーバー。リスナースレッドを起動して常駐する。
                spawn_listener(app.clone(), pipe_name, handle);
                decide_role(true)
            }
            None => {
                // ロック獲得失敗＝既存サーバーあり。クライアントとして引数を転送して終了する。
                forward_to_server(&pipe_name, forward_args);
                decide_role(false)
            }
        }
    }

    /// CreateNamedPipe を FIRST_PIPE_INSTANCE で生成する。既に存在すれば失敗（None）＝クライアント役割。
    fn create_server_pipe(pipe_name: &str) -> Option<HANDLE> {
        let name_w = to_wide(pipe_name);
        let mut sa = security_attributes()?;
        // SAFETY: name_w は NUL 終端。sa はこの関数スコープで有効。
        let handle = unsafe {
            CreateNamedPipeW(
                name_w.as_ptr(),
                PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1, // 同時接続は 1（クライアントは即書込→切断する短命接続）。
                READ_BUF as u32,
                READ_BUF as u32,
                0,
                &mut sa.attrs,
            )
        };
        // SDDL から確保した security descriptor を解放する（成否に関わらず）。
        if !sa.descriptor.is_null() {
            // SAFETY: descriptor は ConvertStringSecurityDescriptor... が確保した LocalAlloc。
            unsafe {
                LocalFree(sa.descriptor as _);
            }
        }
        if handle == INVALID_HANDLE_VALUE {
            None
        } else {
            Some(handle)
        }
    }

    /// サーバーのリスナースレッド。クライアント接続を待ち→受信→core 検証→`emit('open-request')`。
    fn spawn_listener(app: AppHandle, pipe_name: String, first_handle: HANDLE) {
        let moved = handle_to_isize(first_handle);
        std::thread::spawn(move || {
            let mut handle = isize_to_handle(moved);
            loop {
                // 接続待ち（ブロッキング。リスナーは専用スレッドなので UI を塞がない）。
                // SAFETY: handle は有効なサーバーパイプ。
                let connected = unsafe { ConnectNamedPipe(handle, ptr::null_mut()) };
                let _ = connected; // 既接続でも 0 が返る。続けて読む。

                if let Some(req) = read_and_validate(handle) {
                    // 受理操作=パスオープン限定。フロントへ「開け」を送り既存ウィンドウを前面化する。
                    let payload: OpenRequestPayload = req.into();
                    // #5: emit より前に、転送パスをアクセス制御へ個別許可する（後続の openFile が通るよう
                    // open-request 受信前に許可登録しておく）。ac 未登録（極端な異常系）でも emit は行う。
                    if let Some(ac) = app.try_state::<crate::access::AccessControl>() {
                        for p in &payload.paths {
                            ac.allow_file(p);
                        }
                    }
                    let _ = app.emit("open-request", &payload);
                    bring_to_front(&app);
                }

                // この接続を切り、次の接続のためにパイプを作り直す（短命接続モデル）。
                // SAFETY: handle は有効。
                unsafe {
                    DisconnectNamedPipe(handle);
                    CloseHandle(handle);
                }
                match create_next_instance(&pipe_name) {
                    Some(h) => handle = h,
                    None => break, // パイプを作り直せない異常時はリスナーを畳む（アプリ本体は継続）。
                }
            }
        });
    }

    /// 接続済みパイプから 1 メッセージを読み、core でスキーマ検証・パス再検証する。
    fn read_and_validate(handle: HANDLE) -> Option<pika_core::ipc::OpenRequest> {
        let mut buf = vec![0u8; READ_BUF];
        let mut read: u32 = 0;
        // SAFETY: handle は有効。buf は READ_BUF バイト。
        let ok = unsafe {
            ReadFile(
                handle,
                buf.as_mut_ptr() as *mut _,
                READ_BUF as u32,
                &mut read,
                ptr::null_mut(),
            )
        };
        if ok == 0 || read == 0 {
            return None;
        }
        buf.truncate(read as usize);
        // 受信長打切り・JSON スキーマ・version・パス再検証はすべて core が担う（信頼境界）。
        parse_incoming_message(&buf).ok()
    }

    /// 既存サーバーのパイプへ接続し、絶対パス正規化済み引数を JSON 転送する。
    fn forward_to_server(pipe_name: &str, forward_args: &[String]) {
        // 転送 JSON は core が組み立てる（version 付与・空要素除去）。-g はここで分解する。
        let (paths, goto) = split_forward_args(forward_args);
        let json = match build_forward_message(&paths, goto) {
            Ok(j) => j,
            Err(_) => return,
        };
        let name_w = to_wide(pipe_name);
        // SAFETY: name_w は NUL 終端。
        let handle = unsafe {
            CreateFileW(
                name_w.as_ptr(),
                GENERIC_WRITE,
                0,
                ptr::null(),
                OPEN_EXISTING,
                0,
                ptr::null_mut(),
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            // BUSY 等は短命接続の競合。リトライは最小限に留める（CLI は即終了が原則）。
            // SAFETY: GetLastError は副作用なし。
            let _ = unsafe { GetLastError() } == ERROR_PIPE_BUSY;
            return;
        }
        // メッセージモードに設定して 1 メッセージとして書き込む。
        let mut mode = PIPE_READMODE_MESSAGE;
        // SAFETY: handle は有効。
        unsafe {
            SetNamedPipeHandleState(handle, &mut mode, ptr::null_mut(), ptr::null_mut());
        }
        let bytes = json.as_bytes();
        let mut written: u32 = 0;
        // SAFETY: handle は有効。bytes は json の寿命内。
        unsafe {
            WriteFile(
                handle,
                bytes.as_ptr() as *const _,
                bytes.len() as u32,
                &mut written,
                ptr::null_mut(),
            );
            CloseHandle(handle);
        }
    }

    /// 転送引数列を `OpenRequest` 用の (paths, goto) に分解する。
    /// `-g <abs>:<行>[:<桁>]` は core の parse_goto_spec で位置を剥がす。
    fn split_forward_args(args: &[String]) -> (Vec<String>, Option<pika_core::ipc::GotoPosition>) {
        let mut paths = Vec::new();
        let mut goto = None;
        let mut i = 0;
        while i < args.len() {
            if args[i] == "-g" {
                if let Some(spec) = args.get(i + 1) {
                    if let Ok(t) = pika_core::cli::parse_goto_spec(spec) {
                        paths.push(t.file);
                        if let Some(line) = t.line {
                            goto = Some(pika_core::ipc::GotoPosition {
                                line,
                                column: t.column,
                            });
                        }
                    }
                }
                i += 2;
                continue;
            }
            paths.push(args[i].clone());
            i += 1;
        }
        (paths, goto)
    }

    /// 次接続用にパイプの追加インスタンスを生成する（FIRST_PIPE_INSTANCE は付けない）。
    fn create_next_instance(pipe_name: &str) -> Option<HANDLE> {
        let name_w = to_wide(pipe_name);
        let mut sa = security_attributes()?;
        // SAFETY: name_w は NUL 終端。sa は関数スコープで有効。
        let handle = unsafe {
            CreateNamedPipeW(
                name_w.as_ptr(),
                PIPE_ACCESS_INBOUND,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                READ_BUF as u32,
                READ_BUF as u32,
                0,
                &mut sa.attrs,
            )
        };
        if !sa.descriptor.is_null() {
            // SAFETY: LocalAlloc 由来。
            unsafe {
                LocalFree(sa.descriptor as _);
            }
        }
        if handle == INVALID_HANDLE_VALUE {
            None
        } else {
            Some(handle)
        }
    }

    /// 既存メインウィンドウを前面化する（要件3.4「既存ウィンドウを前面化」）。
    fn bring_to_front(app: &AppHandle) {
        if let Some(win) = app.get_webview_window("main") {
            let _ = win.unminimize();
            let _ = win.show();
            let _ = win.set_focus();
        }
    }

    /// SECURITY_ATTRIBUTES と、それが指す security descriptor のハンドルをまとめて返す。
    struct OwnedSa {
        attrs: SECURITY_ATTRIBUTES,
        descriptor: *mut std::ffi::c_void,
    }

    /// ユーザー限定 DACL を持つ SECURITY_ATTRIBUTES を SDDL から組み立てる（design doc 15章-9）。
    fn security_attributes() -> Option<OwnedSa> {
        let sddl_w = to_wide(PIPE_SDDL);
        let mut psd: *mut std::ffi::c_void = ptr::null_mut();
        // SAFETY: sddl_w は NUL 終端。psd は成功時に LocalAlloc されたディスクリプタを受ける。
        let ok = unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl_w.as_ptr(),
                SDDL_REVISION_1,
                &mut psd,
                ptr::null_mut(),
            )
        };
        if ok == 0 || psd.is_null() {
            return None;
        }
        let attrs = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: psd,
            bInheritHandle: 0,
        };
        Some(OwnedSa {
            attrs,
            descriptor: psd,
        })
    }

    /// 現在のプロセストークンからユーザー SID を取り出し文字列化する（`S-1-5-...`）。
    fn current_user_sid() -> Option<String> {
        // SAFETY: 現プロセスの擬似ハンドル（閉じない）。
        let process = unsafe { GetCurrentProcess() };
        let mut token: HANDLE = ptr::null_mut();
        // SAFETY: token は成功時のみ有効。
        let opened = unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut token) };
        if opened == 0 {
            return None;
        }
        // 必要サイズを問い合わせる。
        let mut size: u32 = 0;
        // SAFETY: 1 回目はサイズ取得目的で NULL バッファ。失敗が想定どおり。
        unsafe {
            GetTokenInformation(token, TokenUser, ptr::null_mut(), 0, &mut size);
        }
        if size == 0 {
            // SAFETY: token は有効。
            unsafe {
                CloseHandle(token);
            }
            return None;
        }
        let mut buf = vec![0u8; size as usize];
        // SAFETY: buf は size バイト。
        let got = unsafe {
            GetTokenInformation(
                token,
                TokenUser,
                buf.as_mut_ptr() as *mut _,
                size,
                &mut size,
            )
        };
        // SAFETY: token は有効。
        unsafe {
            CloseHandle(token);
        }
        if got == 0 {
            return None;
        }
        // buf 先頭は TOKEN_USER。User.Sid を文字列化する。
        // SAFETY: buf は TOKEN_USER として有効に満たされている。
        let token_user = unsafe { &*(buf.as_ptr() as *const TOKEN_USER) };
        let mut sid_str: *mut u16 = ptr::null_mut();
        // SAFETY: User.Sid は有効な SID ポインタ。sid_str は LocalAlloc される。
        let conv = unsafe { ConvertSidToStringSidW(token_user.User.Sid, &mut sid_str) };
        if conv == 0 || sid_str.is_null() {
            return None;
        }
        let s = wide_ptr_to_string(sid_str);
        // SAFETY: sid_str は LocalAlloc 由来。
        unsafe {
            LocalFree(sid_str as _);
        }
        Some(s)
    }

    /// UTF-8 → NUL 終端 UTF-16（Win32 境界変換）。
    fn to_wide(s: &str) -> Vec<u16> {
        std::ffi::OsStr::new(s)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    }

    /// NUL 終端ワイド文字列ポインタを String へ。
    fn wide_ptr_to_string(p: *const u16) -> String {
        let mut len = 0usize;
        // SAFETY: p は NUL 終端のワイド文字列。
        unsafe {
            while *p.add(len) != 0 {
                len += 1;
            }
            let slice = std::slice::from_raw_parts(p, len);
            String::from_utf16_lossy(slice)
        }
    }
}
