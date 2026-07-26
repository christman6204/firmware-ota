#!/usr/bin/env python3
"""
STM32 OTA 产线工具 v2.1
"""

import os, sys, json, threading, subprocess
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hex_utils import (hex_to_binary, hex_to_segments, merge_hex_files,
                        merge_hex_to_hex, verify_merged_bin, read_fw_version_from_hex)
from encrypt_firmware import encrypt_firmware

# ── Apple-style palette ───────────────────────────────
C_BG        = "#f2f2f7"
C_CARD      = "#ffffff"
C_PRIMARY   = "#0071e3"
C_PRIMARY_H = "#0077ed"
C_TEXT      = "#1d1d1f"
C_SUBTEXT   = "#86868b"
C_BORDER    = "#d2d2d7"
C_LOG_BG    = "#1d1d1f"
C_LOG_FG    = "#e5e5e7"
C_LOG_OK    = "#30d158"
C_LOG_WARN  = "#ffd60a"
C_LOG_ERR   = "#ff453a"
C_LOG_DIM   = "#6e6e73"

FONT        = ("Microsoft YaHei UI", 11, "bold")
FONT_SM     = ("Microsoft YaHei UI", 10)
FONT_BOLD   = ("Microsoft YaHei UI", 12, "bold")
FONT_TITLE  = ("Microsoft YaHei UI", 15, "bold")
FONT_MONO   = ("Consolas", 16)

PAD_CARD_X  = 20
PAD_CARD_Y  = 16
GAP         = 10

def fmt_size(n):
    if n < 1024*1024: return f"{n/1024:.1f} KB"
    return f"{n/1024/1024:.2f} MB"

# ── helpers ───────────────────────────────────────────
def _card(parent):
    """白底卡片，返回内部 frame"""
    f = tk.Frame(parent, bg=C_CARD, highlightthickness=1, highlightbackground=C_BORDER)
    f.pack(fill=tk.X, pady=(0, GAP))
    b = tk.Frame(f, bg=C_CARD)
    b.pack(fill=tk.X, padx=PAD_CARD_X, pady=PAD_CARD_Y)
    return b

def _row(parent, label, w=9):
    """标签 + 输入框"""
    r = tk.Frame(parent, bg=C_CARD)
    r.pack(fill=tk.X, pady=(0, 5))
    tk.Label(r, text=label, width=w, anchor=tk.W, font=FONT, bg=C_CARD, fg=C_TEXT).pack(side=tk.LEFT)
    e = tk.Entry(r, font=FONT, bg="#f5f5f7", fg=C_TEXT, relief=tk.FLAT, bd=0,
                 highlightthickness=1, highlightbackground=C_BORDER,
                 highlightcolor=C_PRIMARY, insertbackground=C_TEXT)
    e.pack(side=tk.LEFT, fill=tk.X, expand=True, ipady=3)
    return e

def _auto_find_pair(path: str, prefix: str) -> str:
    """在同目录下自动查找配对文件 (如选了 APP*.hex → 找 BT*.hex)"""
    if not path or not os.path.exists(path):
        return ""
    d = os.path.dirname(path)
    matches = [f for f in os.listdir(d) if f.lower().startswith(prefix.lower()) and f.lower().endswith(".hex")]
    if len(matches) == 1:
        return os.path.join(d, matches[0])
    return ""

def _btn_row(parent):
    r = tk.Frame(parent, bg=C_CARD); r.pack(fill=tk.X, pady=(6,0))
    return r

def _primary_btn(parent, text, cmd):
    b = tk.Button(parent, text=text, font=("Microsoft YaHei UI", 14, "bold"),
                  bg=C_PRIMARY, fg="white", activebackground=C_PRIMARY_H,
                  activeforeground="white", relief=tk.FLAT, padx=134, pady=9,
                  cursor="hand2", bd=0, command=cmd)
    b.pack(side=tk.RIGHT)
    return b

def _small_btn(parent, text, cmd):
    b = tk.Button(parent, text=text, font=("Microsoft YaHei UI", 12, "bold"),
                  bg="#e8e8ed", fg=C_TEXT, activebackground="#d2d2d7",
                  activeforeground=C_TEXT, relief=tk.RAISED, bd=1,
                  padx=54, pady=7, cursor="hand2", command=cmd)
    b.pack(side=tk.RIGHT, padx=(4,0))
    return b

def _browse_btn(parent, entry, title, ft):
    _small_btn(parent, "浏览",
        lambda: (p := filedialog.askopenfilename(title=title, filetypes=ft)) and
                (entry.delete(0,tk.END), entry.insert(0,p)))

def _dir_btn(parent, entry, on_save=None):
    def pick():
        p = filedialog.askdirectory(title="选择输出目录")
        if p:
            entry.delete(0, tk.END)
            entry.insert(0, p)
            if on_save: on_save()
    _small_btn(parent, "选择", pick)

def _log(parent, h):
    f = _card(parent)
    log = tk.Text(f, height=h, state=tk.DISABLED, font=FONT_MONO,
                  bg=C_LOG_BG, fg=C_LOG_FG, insertbackground="white",
                  relief=tk.FLAT, padx=14, pady=10, borderwidth=0)
    log.pack(fill=tk.BOTH, expand=True)
    log.tag_config("ok", foreground=C_LOG_OK)
    log.tag_config("warn", foreground=C_LOG_WARN)
    log.tag_config("err", foreground=C_LOG_ERR)
    log.tag_config("dim", foreground=C_LOG_DIM)
    return log

def _append(w, msg, tag=None):
    w.configure(state=tk.NORMAL); w.insert(tk.END, msg+"\n", tag)
    w.see(tk.END); w.configure(state=tk.DISABLED); w.update_idletasks()

def _clear(w):
    w.configure(state=tk.NORMAL); w.delete("1.0",tk.END); w.configure(state=tk.DISABLED)

# ================================================================
class App:
    def __init__(self, r):
        self.root = r
        r.title("STM32 OTA 产线工具")
        r.geometry("780x780")
        r.minsize(700, 680)
        r.configure(bg=C_BG)

        s = ttk.Style()
        s.theme_use("clam")
        s.configure("TNotebook", background=C_BG, borderwidth=0, tabmargins=(0,0,0,0))
        s.configure("TNotebook.Tab", font=("Microsoft YaHei UI", 12, "bold"), padding=(28,12),
                    borderwidth=0, background=C_BG, foreground=C_TEXT)
        s.map("TNotebook.Tab", background=[("selected",C_CARD)], foreground=[("selected",C_SUBTEXT)])
        s.configure("TRadiobutton", background=C_CARD, font=FONT, foreground=C_TEXT)
        s.configure("TCheckbutton", background=C_CARD, font=FONT, foreground=C_TEXT)

        self.aes_key = None; self.hmac_key = None; self.device_key = None
        self._key_path = None  # 上次加载的密钥文件路径

        # 配置文件路径
        tool_dir = os.path.dirname(os.path.abspath(__file__))
        self._config_path = os.path.join(tool_dir, ".factory_tool_config.json")

        self._build()
        self._auto_load_keys()  # 启动时自动加载上次的密钥

    def _build(self):
        # ── header ──
        h = tk.Frame(self.root, bg=C_CARD, height=60, highlightthickness=1, highlightbackground=C_BORDER)
        h.pack(fill=tk.X)
        h.pack_propagate(False)
        tk.Label(h, text="  STM32 OTA 产线工具", font=FONT_TITLE, bg=C_CARD, fg=C_TEXT).pack(side=tk.LEFT, padx=(20,4))
        self.key_lbl = tk.Label(h, text="| 密钥未加载", font=FONT_SM, bg=C_CARD, fg=C_LOG_WARN)
        self.key_lbl.pack(side=tk.LEFT)

        # ── body ──
        body = tk.Frame(self.root, bg=C_BG, padx=GAP+2, pady=GAP)
        body.pack(fill=tk.BOTH, expand=True)

        self.nb = ttk.Notebook(body)
        self.nb.pack(fill=tk.BOTH, expand=True)

        t1 = tk.Frame(self.nb, bg=C_BG, padx=12, pady=12)
        self.nb.add(t1, text="  加密  ")
        self._tab1(t1)

        t2 = tk.Frame(self.nb, bg=C_BG, padx=12, pady=12)
        self.nb.add(t2, text="  合并  ")
        self._tab2(t2)

        tk0 = tk.Frame(self.nb, bg=C_BG, padx=12, pady=12)
        self.nb.add(tk0, text="  密钥  ")
        self._prev_tab = 0  # 记录上一个 tab，用于密码错误时切回
        self.nb.bind("<<NotebookTabChanged>>", self._on_tab_change)
        self._tab0(tk0)

    # ── Tab 0: 密钥 ────────────────────────────────────
    def _tab0(self, p):
        self.t0_show_keys = False
        self.t0_unlocked = False  # Tab 是否已解锁
        self.t0_built = False    # 内容是否已构建

        # 锁屏
        self.t0_lock = tk.Frame(p, bg=C_BG)
        self.t0_lock.pack(fill=tk.BOTH, expand=True)

        msg = tk.Frame(self.t0_lock, bg=C_BG)
        msg.pack(pady=(60, 12))
        tk.Label(msg, text="密钥管理", font=FONT_TITLE, bg=C_BG, fg=C_TEXT).pack()
        tk.Label(msg, text="请输入授权密码以解锁密钥操作", font=FONT_SM, bg=C_BG, fg=C_SUBTEXT).pack(pady=(4, 10))
        e = tk.Entry(msg, font=("Microsoft YaHei UI", 14), show="*",
                     justify=tk.CENTER, width=14)
        e.pack()
        self.t0_hint = tk.Label(msg, text="", font=FONT_SM, bg=C_BG, fg=C_LOG_ERR)
        self.t0_hint.pack(pady=(4, 0))
        e.focus_set()

        def unlock(ev=None):
            if e.get() == "225219":
                self.t0_hint.configure(text="")
                self.t0_unlocked = True
                self.t0_lock.pack_forget()
                if not self.t0_built:
                    self._build_tab0_content()
                    self.t0_built = True
                else:
                    self.t0_content.pack(fill=tk.BOTH, expand=True)
            else:
                e.delete(0, tk.END)
                self.t0_hint.configure(text="密码错误")

        e.bind("<Return>", unlock)
        tk.Button(msg, text="解锁", font=("Microsoft YaHei UI", 11, "bold"),
                  bg=C_PRIMARY, fg="white", relief=tk.FLAT, padx=28, pady=5,
                  cursor="hand2", command=unlock).pack(pady=(10, 0))

        # 实际内容 (初始隐藏)
        self.t0_content = tk.Frame(p, bg=C_BG)

    def _build_tab0_content(self):
        """构建密钥 Tab 的实际内容 (解锁后调用)"""
        p = self.t0_content
        p.pack(fill=tk.BOTH, expand=True)

        c = _card(p)
        tk.Label(c, text="主密钥管理", font=FONT_BOLD, bg=C_CARD, fg=C_TEXT).pack(anchor=tk.W, pady=(0,4))

        info = tk.Frame(c, bg=C_CARD)
        info.pack(fill=tk.X, pady=(0, 8))
        tk.Label(info, text="主密钥在产线生成一次，离线加密保管，同时编译进 Bootloader。",
                 font=FONT_SM, bg=C_CARD, fg=C_SUBTEXT, wraplength=580, justify=tk.LEFT).pack(anchor=tk.W)

        r = tk.Frame(c, bg=C_CARD); r.pack(fill=tk.X, pady=(0, 10))
        _small_btn(r, "生成新密钥", self._gen_keys)
        _small_btn(r, "加载密钥文件", self._load_keys)

        c2 = _card(p)
        title_row = tk.Frame(c2, bg=C_CARD)
        title_row.pack(fill=tk.X, pady=(0, 4))
        tk.Label(title_row, text="当前密钥状态", font=FONT_BOLD, bg=C_CARD, fg=C_TEXT).pack(side=tk.LEFT)
        self.t0_btn_show = tk.Button(title_row, text="显示密钥", font=FONT_SM,
            bg="#e8e8ed", fg=C_TEXT, activebackground="#d2d2d7",
            relief=tk.RAISED, bd=1, padx=14, pady=4, cursor="hand2",
            command=self._toggle_show_keys)
        self.t0_btn_show.pack(side=tk.RIGHT)

        self.t0_status = tk.Text(c2, height=6, state=tk.DISABLED, font=FONT_MONO,
                                  bg=C_LOG_BG, fg=C_LOG_FG, relief=tk.FLAT,
                                  padx=12, pady=10, borderwidth=0)
        self.t0_status.pack(fill=tk.X)
        self.t0_status.tag_config("ok", foreground=C_LOG_OK)
        self.t0_status.tag_config("warn", foreground=C_LOG_WARN)
        self.t0_status.tag_config("dim", foreground=C_LOG_DIM)
        self.t0_status.tag_config("hidden", foreground=C_LOG_BG)
        self._update_key_status()

    def _on_tab_change(self, event):
        """离开密钥 tab 时重新上锁"""
        cur = self.nb.index(self.nb.select())
        if self._prev_tab == 2 and cur != 2:
            # 离开密钥 Tab，重新上锁
            self._relock_t0()
        self._prev_tab = cur

    def _relock_t0(self):
        """重新锁定密钥 Tab"""
        if hasattr(self, 't0_unlocked') and self.t0_unlocked:
            self.t0_content.pack_forget()
            self.t0_lock.pack(fill=tk.BOTH, expand=True)
            self.t0_unlocked = False
            self.t0_hint.configure(text="")
            for child in self.t0_lock.winfo_children():
                for sub in child.winfo_children():
                    if isinstance(sub, tk.Entry):
                        sub.delete(0, tk.END)
                        sub.focus_set()

    def _toggle_show_keys(self):
        """切换密钥显示/隐藏"""
        if self.t0_show_keys:
            self.t0_show_keys = False
            self.t0_btn_show.configure(text="显示密钥")
        else:
            if not self._check_pwd():
                return
            self.t0_show_keys = True
            self.t0_btn_show.configure(text="隐藏密钥")
        self._update_key_status()

    def _update_key_status(self):
        """刷新密钥状态显示"""
        if not hasattr(self, 't0_status'):
            return
        self.t0_status.configure(state=tk.NORMAL)
        self.t0_status.delete("1.0", tk.END)
        if self.aes_key:
            if self.t0_show_keys:
                self.t0_status.insert(tk.END, "密钥文件:  ", "dim")
                self.t0_status.insert(tk.END, f"{self._key_path}\n", "ok")
                self.t0_status.insert(tk.END, "文件名:    ", "dim")
                self.t0_status.insert(tk.END, f"{os.path.basename(self._key_path)}\n\n", "ok")
                self.t0_status.insert(tk.END, "AES 密钥:  ", "dim")
                self.t0_status.insert(tk.END, f"{self.aes_key.hex()}\n", "ok")
                self.t0_status.insert(tk.END, "HMAC 密钥: ", "dim")
                self.t0_status.insert(tk.END, f"{self.hmac_key.hex()}\n", "ok")
                self.t0_status.insert(tk.END, "Device Key:", "dim")
                self.t0_status.insert(tk.END, f"{self.device_key.hex()}", "ok")
            else:
                self.t0_status.insert(tk.END, "密钥文件:  ", "dim")
                self.t0_status.insert(tk.END, "********************\n", "hidden")
                self.t0_status.insert(tk.END, "文件名:    ", "dim")
                self.t0_status.insert(tk.END, "********\n\n", "hidden")
                self.t0_status.insert(tk.END, "AES 密钥:  ", "dim")
                self.t0_status.insert(tk.END, "****************\n", "hidden")
                self.t0_status.insert(tk.END, "HMAC 密钥: ", "dim")
                self.t0_status.insert(tk.END, "****************\n", "hidden")
                self.t0_status.insert(tk.END, "Device Key:", "dim")
                self.t0_status.insert(tk.END, "****************", "hidden")
                self.t0_status.insert(tk.END, "****************", "hidden")
            self.t0_status.insert(tk.END, "\n\n状态: 已加载, 加密功能可用", "ok")
        else:
            self.t0_status.insert(tk.END, "尚未加载主密钥。\n\n", "warn")
            self.t0_status.insert(tk.END, "请点击「生成新密钥」产线生成密钥对,\n", "dim")
            self.t0_status.insert(tk.END, "或「加载密钥文件」使用已有密钥。", "dim")
        self.t0_status.configure(state=tk.DISABLED)

    # ── Tab 1: 加密 ────────────────────────────────────
    def _tab1(self, p):
        c = _card(p)
        tk.Label(c, text="固件输入", font=FONT_BOLD, bg=C_CARD, fg=C_TEXT).pack(anchor=tk.W, pady=(0,4))
        r1 = tk.Frame(c, bg=C_CARD); r1.pack(fill=tk.X, pady=(0,5))
        self.t1_hex = _row(c, "App HEX")
        self.t1_hex.bind("<FocusOut>", lambda e: self._auto_version())

        def browse_app():
            while True:
                p = filedialog.askopenfilename(title="选择 App HEX (必须以 APP 开头)",
                    filetypes=[("HEX","*.hex"),("All","*.*")])
                if not p:
                    return
                bn = os.path.basename(p).upper()
                if bn.startswith("APP"):
                    self.t1_hex.delete(0, tk.END)
                    self.t1_hex.insert(0, p)
                    self._auto_version()
                    self._save_config()
                    return
                else:
                    messagebox.showwarning("文件错误",
                        f"文件名 \"{os.path.basename(p)}\" 不是以 APP 开头。\n\n"
                        "请重新选择 APP 固件文件。")

        _small_btn(r1, "浏览", browse_app)

        r2 = tk.Frame(c, bg=C_CARD); r2.pack(fill=tk.X, pady=(0,5))
        self.t1_version = _row(c, "版本号")
        self.t1_version.configure(state=tk.DISABLED, disabledbackground="#e8f0fe",
                                  disabledforeground=C_PRIMARY, font=FONT_BOLD)
        self.t1_vhint = tk.Label(r2, text="← 自动从 HEX 读取", font=FONT_SM,
                                 bg=C_CARD, fg=C_SUBTEXT)
        self.t1_vhint.pack(side=tk.RIGHT)

        r3 = tk.Frame(c, bg=C_CARD); r3.pack(fill=tk.X)
        self.t1_outdir = _row(c, "输出目录")
        _dir_btn(r3, self.t1_outdir, self._save_config)

        r4 = _btn_row(c)
        self.btn_t1 = _primary_btn(r4, "加密", self._run_enc)

        self.log1 = _log(p, 18)

    def _auto_version(self):
        """从 HEX 文件中自动读取固件版本号"""
        hp = self.t1_hex.get().strip()
        if not hp or not os.path.exists(hp):
            return
        try:
            maj, min_ = read_fw_version_from_hex(hp)
            if maj == 0 and min_ == 0:
                self.t1_vhint.configure(text="← HEX 中未找到版本号 (0x0800F800)", fg=C_LOG_WARN)
            else:
                ver = f"{maj}.{min_}"
                self.t1_version.configure(state=tk.NORMAL)
                self.t1_version.delete(0, tk.END)
                self.t1_version.insert(0, ver)
                self.t1_version.configure(state=tk.DISABLED)
                self.t1_vhint.configure(text=f"← 自动读取 v{ver}", fg=C_LOG_OK)
        except Exception:
            self.t1_vhint.configure(text="← 读取失败", fg=C_LOG_ERR)

    # ── Tab 2 ──────────────────────────────────────────
    def _tab2(self, p):
        c = _card(p)

        def browse_hex_t2():
            while True:
                files = filedialog.askopenfilenames(
                    title="同时选择 BT HEX 和 App HEX 两个文件 (Ctrl+点击多选)",
                    filetypes=[("HEX","*.hex"),("All","*.*")])
                if not files:
                    return
                if len(files) != 2:
                    messagebox.showwarning("选择错误",
                        f"需要选择 2 个文件 (当前选了 {len(files)} 个)。\n\n"
                        "请同时选中 BT*.hex 和 APP*.hex 两个文件。")
                    continue

                bt_file = None; app_file = None
                for f in files:
                    bn = os.path.basename(f).upper()
                    if bn.startswith("BT"):
                        bt_file = f
                    elif bn.startswith("APP"):
                        app_file = f

                if not bt_file:
                    messagebox.showwarning("文件错误",
                        "缺少 BT 固件文件。\n\n"
                        "请确保选中的两个文件中有一个文件名以 BT 开头。")
                    continue
                if not app_file:
                    messagebox.showwarning("文件错误",
                        "缺少 APP 固件文件。\n\n"
                        "请确保选中的两个文件中有一个文件名以 APP 开头。")
                    continue

                # 设备型号一致性校验
                bt_name = os.path.splitext(os.path.basename(bt_file).upper())[0]
                ap_name = os.path.splitext(os.path.basename(app_file).upper())[0]
                bt_model = bt_name.split("_")[1] if "_" in bt_name else ""
                ap_model = ap_name.split("_")[1] if "_" in ap_name else ""
                if not bt_model or not ap_model:
                    messagebox.showwarning("文件错误",
                        "文件名无法识别设备型号。\n\n格式: BT_<型号>_xxx.hex  /  APP_<型号>_xxx.hex")
                    continue
                if bt_model != ap_model:
                    messagebox.showwarning("文件错误",
                        f"设备型号不一致。\n\nBT:  {bt_model}\nApp: {ap_model}\n\n请选择同一设备型号的文件。")
                    continue

                for e, v in [(self.t2_bl, bt_file), (self.t2_app, app_file)]:
                    e.configure(state=tk.NORMAL)
                    e.delete(0, tk.END)
                    e.insert(0, v)
                    e.configure(state=tk.DISABLED)
                self._save_config()
                return

        # 标题行: "输入文件" + 浏览按钮
        title_row = tk.Frame(c, bg=C_CARD)
        title_row.pack(fill=tk.X, pady=(0, 4))
        tk.Label(title_row, text="输入文件", font=FONT_BOLD, bg=C_CARD, fg=C_TEXT).pack(side=tk.LEFT)
        _small_btn(title_row, "浏览", browse_hex_t2)

        self.t2_bl = _row(c, "BT HEX")
        self.t2_bl.configure(state=tk.DISABLED, disabledbackground="#f0f0f5",
                             disabledforeground=C_TEXT, font=FONT)
        self.t2_app = _row(c, "App HEX")
        self.t2_app.configure(state=tk.DISABLED, disabledbackground="#f0f0f5",
                              disabledforeground=C_TEXT, font=FONT)

        # 输出目录 + 输出选项合并为一行
        r3 = tk.Frame(c, bg=C_CARD); r3.pack(fill=tk.X, pady=(2,0))
        self.t2_outdir = _row(c, "输出目录")
        _dir_btn(r3, self.t2_outdir, self._save_config)

        # 输出选项: 紧凑单行
        c2 = _card(p)
        top = tk.Frame(c2, bg=C_CARD); top.pack(fill=tk.X)

        tk.Label(top, text="格式", font=FONT, bg=C_CARD, fg=C_TEXT).pack(side=tk.LEFT)
        self.t2_fmt = tk.StringVar(value="bin")
        ttk.Radiobutton(top, text="BIN", variable=self.t2_fmt,
                        value="bin").pack(side=tk.LEFT, padx=8)
        ttk.Radiobutton(top, text="HEX", variable=self.t2_fmt,
                        value="hex").pack(side=tk.LEFT, padx=8)
        ttk.Separator(top, orient=tk.VERTICAL).pack(side=tk.LEFT, padx=10, fill=tk.Y)
        tk.Label(top, text="填充", font=FONT_SM, bg=C_CARD, fg=C_SUBTEXT).pack(side=tk.LEFT, padx=(6,2))
        self.t2_fill = tk.StringVar(value="0xFF")
        ttk.Radiobutton(top, text="0xFF", variable=self.t2_fill, value="0xFF").pack(side=tk.LEFT, padx=4)
        ttk.Radiobutton(top, text="0x00", variable=self.t2_fill, value="0x00").pack(side=tk.LEFT, padx=4)

        self.btn_t2 = _primary_btn(top, "合并", self._run_merge)

        self.log2 = _log(p, 22)

    # ── 密钥 (密码保护) ────────────────────────────────
    def _check_pwd(self) -> bool:
        """弹出密码验证对话框，正确返回 True"""
        dlg = tk.Toplevel(self.root)
        dlg.title("密钥操作验证")
        dlg.geometry("320x160")
        dlg.configure(bg=C_CARD)
        dlg.resizable(False, False)
        dlg.transient(self.root)
        dlg.grab_set()

        # 居中
        dlg.update_idletasks()
        x = self.root.winfo_x() + (self.root.winfo_width() - 320) // 2
        y = self.root.winfo_y() + (self.root.winfo_height() - 160) // 2
        dlg.geometry(f"+{x}+{y}")

        tk.Label(dlg, text="密钥操作为敏感操作", font=FONT_BOLD,
                 bg=C_CARD, fg=C_TEXT).pack(pady=(20, 4))
        tk.Label(dlg, text="请输入授权密码", font=FONT_SM,
                 bg=C_CARD, fg=C_SUBTEXT).pack()

        e = tk.Entry(dlg, font=("Microsoft YaHei UI", 14), show="*",
                     justify=tk.CENTER, width=14)
        e.pack(pady=(10, 14))
        e.focus_set()

        result = [False]

        def verify(ev=None):
            if e.get() == "225219":
                result[0] = True
                dlg.destroy()
            else:
                e.delete(0, tk.END)
                e.configure(bg="#fef2f2")
                e.after(300, lambda: e.configure(bg="white"))

        e.bind("<Return>", verify)

        fr = tk.Frame(dlg, bg=C_CARD)
        fr.pack()
        tk.Button(fr, text="确认", font=("Microsoft YaHei UI", 11, "bold"),
                  bg=C_PRIMARY, fg="white", relief=tk.FLAT, padx=20, pady=4,
                  cursor="hand2", command=verify).pack(side=tk.LEFT, padx=4)
        tk.Button(fr, text="取消", font=FONT,
                  bg="#e8e8ed", fg=C_TEXT, relief=tk.FLAT, padx=20, pady=4,
                  cursor="hand2", command=dlg.destroy).pack(side=tk.LEFT, padx=4)

        self.root.wait_window(dlg)
        return result[0]

    def _save_config(self):
        """保存所有可记忆设置到本地配置"""
        try:
            cfg = {
                "key_file": self._key_path,
                "t1_hex": self.t1_hex.get().strip(),
                "t1_outdir": self.t1_outdir.get().strip(),
                "t2_bl": self.t2_bl.get().strip(),
                "t2_app": self.t2_app.get().strip(),
                "t2_outdir": self.t2_outdir.get().strip(),
            }
            with open(self._config_path, "w", encoding="utf-8") as f:
                json.dump(cfg, f)
        except Exception:
            pass

    def _auto_load_keys(self):
        """启动时自动加载配置：密钥 + 上次选择的文件路径"""
        try:
            if not os.path.exists(self._config_path):
                return
            with open(self._config_path, encoding="utf-8") as f:
                cfg = json.load(f)

            # 恢复文件路径 (合并 Tab 的 Entry 是 DISABLED 状态, 需要临时启用)
            for key, attr in [("t1_hex", self.t1_hex), ("t1_outdir", self.t1_outdir),
                              ("t2_bl", self.t2_bl), ("t2_app", self.t2_app),
                              ("t2_outdir", self.t2_outdir)]:
                val = cfg.get(key, "")
                if val:
                    was_disabled = str(attr.cget("state")) == "disabled"
                    if was_disabled:
                        attr.configure(state=tk.NORMAL)
                    attr.delete(0, tk.END)
                    attr.insert(0, val)
                    if was_disabled:
                        attr.configure(state=tk.DISABLED)
            # 恢复后自动读取版本号
            if cfg.get("t1_hex"):
                self._auto_version()

            # 恢复密钥
            kp = cfg.get("key_file", "")
            if not kp or not os.path.exists(kp):
                return
            with open(kp, encoding="utf-8") as f:
                d = json.load(f)
            ak = bytes.fromhex(d["aes_key"]); hk = bytes.fromhex(d["hmac_key"])
            dk = bytes.fromhex(d.get("master_device_key", "00"*32))
            if len(ak) != 32 or len(hk) != 32:
                return
            self.aes_key = ak; self.hmac_key = hk; self.device_key = dk; self._key_path = kp
            self.key_lbl.configure(text="| 密钥已加载", fg=C_LOG_OK)
            self._update_key_status()
        except Exception:
            pass

    def _load_keys(self):
        p = filedialog.askopenfilename(title="加载密钥", filetypes=[("JSON","*.json"),("All","*.*")])
        if not p: return
        try:
            with open(p,encoding="utf-8") as f: d=json.load(f)
            ak=bytes.fromhex(d["aes_key"]); hk=bytes.fromhex(d["hmac_key"])
            dk=bytes.fromhex(d.get("master_device_key", "00"*32))
            if len(ak)!=32 or len(hk)!=32: raise ValueError("长度错误")
            self.aes_key=ak; self.hmac_key=hk; self.device_key=dk; self._key_path = p
            self._save_config()
            self.key_lbl.configure(text="| 密钥已加载", fg=C_LOG_OK)
            self._update_key_status()
            _append(self.log1, "已加载 "+os.path.basename(p), "ok")
            _append(self.log2, "密钥已就绪, 加密 Tab 可用", "ok")
        except Exception as e:
            messagebox.showerror("加载失败", str(e))

    def _gen_keys(self):
        p = filedialog.asksaveasfilename(title="保存密钥", defaultextension=".json", filetypes=[("JSON","*.json")])
        if not p: return
        try:
            r = subprocess.run([sys.executable,
                os.path.join(os.path.dirname(__file__),"gen_master_keys.py"),
                "--quiet","-o",p], capture_output=True, text=True, check=True)
            d=json.loads(r.stdout)
            dk=bytes.fromhex(d.get("master_device_key", "00"*32))
            self.aes_key=bytes.fromhex(d["aes_key"]); self.hmac_key=bytes.fromhex(d["hmac_key"])
            self.device_key=dk; self._key_path = p
            self._save_config()
            self.key_lbl.configure(text="| 密钥已加载", fg=C_LOG_OK)
            self._update_key_status()
            _append(self.log1, "新密钥 -> "+p, "ok")
        except Exception as e:
            messagebox.showerror("生成失败", str(e))

    # ── 加密 ────────────────────────────────────────────
    def _run_enc(self):
        if not self.aes_key: messagebox.showwarning("缺少密钥","请先加载密钥"); return
        hp=self.t1_hex.get().strip(); ver=self.t1_version.get().strip(); od=self.t1_outdir.get().strip()
        if not hp: messagebox.showwarning("缺少输入","选 App HEX"); return
        if not os.path.basename(hp).upper().startswith("APP"):
            messagebox.showwarning("文件错误",
                f"文件名 \"{os.path.basename(hp)}\" 不是以 APP 开头。\n请重新选择 APP 固件文件。")
            return
        if not os.path.exists(hp): messagebox.showerror("不存在",hp); return
        if not od:
            od=os.path.dirname(hp); self.t1_outdir.delete(0,tk.END); self.t1_outdir.insert(0,od)
            self._save_config()
        if not os.path.isdir(od): messagebox.showerror("目录不存在",od); return
        self.btn_t1.configure(state=tk.DISABLED, text="处理中...")
        _clear(self.log1)
        threading.Thread(target=self._do_enc, args=(hp,ver,od), daemon=True).start()

    def _do_enc(self, hp, ver, od):
        try:
            bn=os.path.splitext(os.path.basename(hp))[0]
            parts=bn.split("_")
            model=parts[1] if len(parts)>1 else ""
            # 版本号从 HEX 内容中读取 (0x0800F800), 不用文件名中的
            out_base=f"UP_{model}_V{ver}" if model else f"UP_V{ver}"
            _append(self.log1, "解析  "+hp, "dim")
            segs = hex_to_segments(hp)
            for s in segs:
                _append(self.log1, f"      0x{s.address:08X} -> 0x{s.address+len(s.data):08X}  {len(s.data):,} B", "dim")

            # 所有段合并为连续二进制，间隙填 0xFF
            seg_start = min(s.address for s in segs)
            seg_end   = max(s.address + len(s.data) for s in segs)
            pt = bytearray([0xFF] * (seg_end - seg_start))
            for s in segs:
                offset = s.address - seg_start
                pt[offset:offset + len(s.data)] = s.data
            pt = bytes(pt)
            flash_addr = seg_start  # Bootloader 按此地址写入
            _append(self.log1, f"      合并: 0x{flash_addr:08X} + {len(pt):,} B", "dim")
            bp=os.path.join(od, f"{out_base}_no_encry.bin")
            with open(bp,"wb") as f: f.write(pt)
            _append(self.log1, f"  -> {os.path.basename(bp)}", "ok")
            _append(self.log1, "加密  AES-256-CTR + HMAC-SHA256", "dim")
            blob,iv,sig=encrypt_firmware(pt, self.aes_key, self.hmac_key)
            op=os.path.join(od, f"{out_base}.bin")
            with open(op,"wb") as f: f.write(blob)
            _append(self.log1, f"  -> {os.path.basename(op)}", "ok")
            _append(self.log1, "─"*48, "dim")
            _append(self.log1, f"  file_size = {len(blob)}", "ok")
            _append(self.log1, f"  iv        = {iv.hex()}", "ok")
            _append(self.log1, f"  hmac      = {sig.hex()}", "ok")
            _append(self.log1, "─"*48, "dim")
        except Exception as e:
            _append(self.log1, f"[ERROR] {e}", "err")
        finally:
            self.root.after(0, lambda: self.btn_t1.configure(state=tk.NORMAL, text="转换并加密"))

    # ── 合并 ────────────────────────────────────────────
    def _run_merge(self):
        bp=self.t2_bl.get().strip(); ap=self.t2_app.get().strip(); od=self.t2_outdir.get().strip()
        if not bp or not ap: messagebox.showwarning("缺少输入","请先浏览选择 BT HEX + App HEX"); return
        # 文件名校验
        bn_bl = os.path.basename(bp).upper(); bn_ap = os.path.basename(ap).upper()
        if not bn_bl.startswith("BT") or not bn_ap.startswith("APP"):
            messagebox.showwarning("文件错误",
                "文件名不合法。BT HEX 必须以 BT 开头，App HEX 必须以 APP 开头。\n请重新选择。")
            return
        # 设备型号一致性校验: BT_<model>_xxx.hex  vs  APP_<model>_xxx.hex
        bl_name = os.path.splitext(bn_bl)[0]; ap_name = os.path.splitext(bn_ap)[0]
        bl_parts = bl_name.split("_"); ap_parts = ap_name.split("_")
        bl_model = bl_parts[1] if len(bl_parts) > 1 else ""
        ap_model = ap_parts[1] if len(ap_parts) > 1 else ""
        if not bl_model or not ap_model:
            messagebox.showwarning("文件错误",
                "文件名无法识别设备型号。\n\n文件名格式: BT_<型号>_xxx.hex  /  APP_<型号>_xxx.hex")
            return
        if bl_model != ap_model:
            messagebox.showwarning("文件错误",
                f"设备型号不一致。\n\nBT:  {bl_model}\nApp: {ap_model}\n\n请选择同一设备型号的文件。")
            return
        if not os.path.exists(bp): messagebox.showerror("不存在",bp); return
        if not os.path.exists(ap): messagebox.showerror("不存在",ap); return
        if not od:
            od=os.path.dirname(ap); self.t2_outdir.delete(0,tk.END); self.t2_outdir.insert(0,od)
            self._save_config()
        if not os.path.isdir(od): messagebox.showerror("目录不存在",od); return
        # 从 APP hex 解析输出名: APP_<model>_*.hex → FA_<model>_V<ver>, 版本从 HEX 内容读取
        bn=os.path.splitext(os.path.basename(ap))[0]; parts=bn.split("_")
        model=parts[1] if len(parts)>1 else ""
        maj,min_=read_fw_version_from_hex(ap)
        ver_str=f"{maj}.{min_}"
        out_base=f"FA_{model}_V{ver_str}" if model else f"FA_V{ver_str}"

        fmt=self.t2_fmt.get(); fb=int(self.t2_fill.get(),16)
        self.btn_t2.configure(state=tk.DISABLED, text="处理中...")
        _clear(self.log2)
        threading.Thread(target=self._do_merge, args=(bp,ap,od,fmt,fb,out_base), daemon=True).start()

    def _do_merge(self, bp, ap, od, fmt, fb, out_base):
        ext = "hex" if fmt == "hex" else "bin"
        try:
            _append(self.log2, f"BT  {bp}", "dim")
            _append(self.log2, f"App {ap}", "dim")
            _append(self.log2, "", "dim")

            bl_segs = hex_to_segments(bp)
            app_segs = hex_to_segments(ap)
            _append(self.log2, "2. 写入 bootloader 段:", "dim")
            for s in bl_segs:
                label = "BL 代码" if s.address == 0x08000000 else ""
                _append(self.log2, f"     0x{s.address:08X}  {label}  {len(s.data):,} B", "dim")
            _append(self.log2, "     0x0800C000  加密ID标记  4 B", "dim")
            _append(self.log2, "3. 写入 app 段:", "dim")
            for s in app_segs:
                label = "APP_INFO" if s.address == 0x0800F800 else "App 代码"
                _append(self.log2, f"     0x{s.address:08X}  {label}  {len(s.data):,} B", "dim")
            _append(self.log2, "", "dim")

            if fmt=="hex":
                op=os.path.join(od, f"{out_base}.hex")
                c=merge_hex_to_hex(bp, ap, op)
                sz=os.path.getsize(op)
                _append(self.log2, f"  -> {os.path.basename(op)}    {sz:,} B    {c} 段", "ok")
                _append(self.log2, "", "dim")
                _append(self.log2, "烧录:  ST-LINK_CLI -P "+op, "dim")
            else:
                op=os.path.join(od, f"{out_base}.bin")
                sz,gaps=merge_hex_files(bp, ap, op, fill_byte=fb, full_flash=False)
                info=verify_merged_bin(op)
                _append(self.log2, f"  -> {os.path.basename(op)}    {sz:,} B", "ok")
                _append(self.log2, f"      BT={'OK' if not info['bl_empty'] else '??'}  "
                    f"App={'OK' if not info['app_empty'] else '??'}", "dim")
                _append(self.log2, "", "dim")
                _append(self.log2, "烧录:  ST-LINK_CLI -P "+op+" 0x08000000", "dim")
            _append(self.log2, "─"*48+"\nOK", "ok")
        except Exception as e:
            _append(self.log2, f"[ERROR] {e}", "err")
        finally:
            self.root.after(0, lambda: self.btn_t2.configure(state=tk.NORMAL, text="合并"))

def main():
    root=tk.Tk()
    App(root)
    root.mainloop()

if __name__=="__main__":
    main()
