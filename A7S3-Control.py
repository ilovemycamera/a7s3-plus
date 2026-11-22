#!/usr/bin/env python3
"""
A7S3-Control.py — Lightweight dark-mode desktop launcher for a7s3-plus (macOS / Linux).
Uses Python's built-in tkinter (no extra GUI packages required).
"""
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

ROOT = Path(__file__).resolve().parent


class A7S3ControlApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('a7s3-plus — Sony A7S III Control Panel')
        self.geometry('820x700')
        self.minsize(700, 580)
        self.configure(bg='#0d1117')
        self.log_q = queue.Queue()
        self.busy = False
        self.sudo_pw = tk.StringVar()
        self.status_var = tk.StringVar(value='Ready. Connect A7S III (FW 5.01) in USB Mass Storage mode.')
        self._configure_style()
        self._build_ui()
        self.after(100, self._poll_queue)

    def _configure_style(self):
        style = ttk.Style(self)
        style.theme_use('clam')
        style.configure('TFrame', background='#0d1117')
        style.configure('Card.TFrame', background='#161b22')
        style.configure('TLabel', background='#0d1117', foreground='#e6edf3', font=('Helvetica', 12))
        style.configure('Title.TLabel', background='#0d1117', foreground='#f0f6fc', font=('Helvetica', 20, 'bold'))
        style.configure('Muted.TLabel', background='#0d1117', foreground='#8b949e', font=('Helvetica', 11))
        style.configure('CardTitle.TLabel', background='#161b22', foreground='#f0f6fc', font=('Helvetica', 13, 'bold'))
        style.configure('CardMuted.TLabel', background='#161b22', foreground='#8b949e', font=('Helvetica', 11))
        style.configure('TButton', padding=(12, 7), font=('Helvetica', 11))
        style.configure('Accent.TButton', background='#238636', foreground='#ffffff', padding=(14, 8), font=('Helvetica', 11, 'bold'))
        style.map('Accent.TButton', background=[('active', '#2ea043')])
        style.configure('Danger.TButton', background='#b62324', foreground='#ffffff', padding=(12, 7))
        style.map('Danger.TButton', background=[('active', '#da3633')])

    def _card(self, parent, title, subtitle=''):
        frame = ttk.Frame(parent, style='Card.TFrame', padding=14)
        ttk.Label(frame, text=title, style='CardTitle.TLabel').pack(anchor='w')
        if subtitle:
            ttk.Label(frame, text=subtitle, style='CardMuted.TLabel', wraplength=740).pack(anchor='w', pady=(2, 10))
        return frame

    def _build_ui(self):
        outer = ttk.Frame(self, padding=18)
        outer.pack(fill='both', expand=True)

        header = ttk.Frame(outer)
        header.pack(fill='x')
        ttk.Label(header, text='a7s3-plus Control', style='Title.TLabel').pack(side='left')
        ttk.Button(header, text='Check USB & Boot Status', command=self.run_check).pack(side='right')
        ttk.Label(outer, textvariable=self.status_var, style='Muted.TLabel').pack(anchor='w', pady=(4, 12))

        auth = self._card(
            outer,
            'Administrator Password (for libusb / USB Mass Storage)',
            'Used only in memory to invoke sudo for install_clean_camera.sh; never saved to disk.'
        )
        row = ttk.Frame(auth, style='Card.TFrame')
        row.pack(fill='x')
        ttk.Entry(row, textvariable=self.sudo_pw, show='*', width=28).pack(side='left')
        ttk.Button(row, text='Verify Camera Connection', command=self.run_check).pack(side='left', padx=10)
        auth.pack(fill='x', pady=(0, 10))

        ram = self._card(
            outer,
            'RAM-Only Camera Loader (Reverts Cleanly on Power Off)',
            'Loads Native 3840x2560 (1:1 SAR) Open Gate 3:2, native Sony menu toggle, BIG6, Variable Shutter, and optional DOOM.'
        )
        r1 = ttk.Frame(ram, style='Card.TFrame')
        r1.pack(fill='x', pady=(0, 8))
        ttk.Button(
            r1, text='Load Native 3840x2560 Open Gate + Cinema', style='Accent.TButton',
            command=lambda: self.run_installer(['og_menu'])
        ).pack(side='left', padx=(0, 8))
        ttk.Button(
            r1, text='Load Full Suite + 1993 DOOM',
            command=lambda: self.run_installer(['doom'])
        ).pack(side='left', padx=(0, 8))
        ttk.Button(
            r1, text='Restore Clean Stock State', style='Danger.TButton',
            command=lambda: self.run_installer(['--recover'])
        ).pack(side='right')

        r2 = ttk.Frame(ram, style='Card.TFrame')
        r2.pack(fill='x')
        ttk.Button(r2, text='Enable Persistence v2 (Auto-Load)', command=lambda: self.run_installer(['--persist'])).pack(side='left', padx=(0, 6))
        ttk.Button(r2, text='Disable Persistence', command=lambda: self.run_installer(['--disarm'])).pack(side='left', padx=(0, 6))
        ttk.Button(r2, text='CP Apply 3:2', command=lambda: self.run_installer(['cp_apply'])).pack(side='left', padx=(0, 6))
        ttk.Button(r2, text='CP Restore 16:9', command=lambda: self.run_installer(['cp_restore'])).pack(side='left')
        ram.pack(fill='x', pady=(0, 10))

        post = self._card(
            outer,
            'Open Gate 3:2 MP4 Inspector & Legacy Tagger (No Camera Needed)',
            'Inspect coded 3840x2560 / 3840x2160 frames & Sony RTMD gyro survival, or tag legacy 3840x2160 clips to tkhd=3840x2560.'
        )
        pr = ttk.Frame(post, style='Card.TFrame')
        pr.pack(fill='x')
        ttk.Button(pr, text='Inspect MP4 / MOV...', command=self.inspect_mp4_dialog).pack(side='left', padx=(0, 8))
        ttk.Button(pr, text='Losslessly Tag 3:2 (3840x2560)...', command=self.tag_mp4_dialog).pack(side='left', padx=(0, 8))
        ttk.Button(pr, text='Copy FFmpeg ProRes/HEVC Cmd...', command=self.ffmpeg_cmd_dialog).pack(side='left')
        post.pack(fill='x', pady=(0, 10))

        log_frame = ttk.Frame(outer)
        log_frame.pack(fill='both', expand=True)
        ttk.Label(log_frame, text='Output Log', style='Muted.TLabel').pack(anchor='w', pady=(0, 4))
        self.log_box = tk.Text(
            log_frame, bg='#010409', fg='#c9d1d9', insertbackground='#ffffff',
            font=('Menlo', 11), relief='flat', wrap='word'
        )
        self.log_box.pack(fill='both', expand=True)

    def _append_log(self, text: str):
        self.log_box.insert('end', text)
        self.log_box.see('end')

    def _poll_queue(self):
        try:
            while True:
                kind, payload = self.log_q.get_nowait()
                if kind == 'log':
                    self._append_log(payload)
                elif kind == 'status':
                    self.status_var.set(payload)
                elif kind == 'done':
                    self.busy = False
        except queue.Empty:
            pass
        self.after(100, self._poll_queue)

    def _run_subprocess(self, label: str, cmd: list, stdin_data: bytes = None):
        if self.busy:
            messagebox.showinfo('Busy', 'Another task is currently running.')
            return
        self.busy = True
        self.status_var.set(f'Running: {label}...')
        self._append_log(f'\n=== {label} ===\n')

        def worker():
            try:
                proc = subprocess.Popen(
                    cmd,
                    cwd=str(ROOT),
                    stdin=subprocess.PIPE if stdin_data is not None else None,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                if stdin_data is not None and proc.stdin is not None:
                    proc.stdin.write(stdin_data)
                    proc.stdin.flush()
                    proc.stdin.close()
                assert proc.stdout is not None
                for raw_line in proc.stdout:
                    self.log_q.put(('log', raw_line.decode('utf-8', 'replace')))
                rc = proc.wait()
                self.log_q.put(('status', f'Finished ({label}) — exit code {rc}'))
            except Exception as exc:
                self.log_q.put(('log', f'[ERROR] {exc}\n'))
                self.log_q.put(('status', f'Failed ({label})'))
            finally:
                self.log_q.put(('done', None))

        threading.Thread(target=worker, daemon=True).start()

    def run_installer(self, args: list):
        pw = self.sudo_pw.get()
        if not pw:
            messagebox.showwarning(
                'Password Required',
                'Enter your macOS/Linux administrator password above so libusb can access the Sony USB device.'
            )
            return
        cmd = ['sudo', '-S', '-E', 'bash', str(ROOT / 'install_clean_camera.sh')] + args
        self._run_subprocess(' '.join(args), cmd, stdin_data=(pw + '\n').encode('utf-8'))

    def run_check(self):
        self.run_installer(['--check'])

    def inspect_mp4_dialog(self):
        path = filedialog.askopenfilename(
            title='Select Sony MP4 / MOV Clip',
            filetypes=[('Video Files', '*.MP4 *.mp4 *.MOV *.mov'), ('All Files', '*.*')]
        )
        if not path:
            return
        self._run_subprocess(
            f'Inspect {Path(path).name}',
            [sys.executable, str(ROOT / 'tools' / 'opengate_mp4_tool.py'), 'inspect', path]
        )

    def tag_mp4_dialog(self):
        src = filedialog.askopenfilename(
            title='Select Open Gate MP4 to Tag as 3:2',
            filetypes=[('Video Files', '*.MP4 *.mp4 *.MOV *.mov'), ('All Files', '*.*')]
        )
        if not src:
            return
        src_p = Path(src)
        dst = filedialog.asksaveasfilename(
            title='Save Lossless 3:2 Copy As',
            initialdir=str(src_p.parent),
            initialfile=f'{src_p.stem}_3x2{src_p.suffix}',
            defaultextension=src_p.suffix,
        )
        if not dst:
            return
        self._run_subprocess(
            f'Tag 3:2 -> {Path(dst).name}',
            [sys.executable, str(ROOT / 'tools' / 'opengate_mp4_tool.py'), 'tag-3x2', src, '-o', dst]
        )

    def ffmpeg_cmd_dialog(self):
        src = filedialog.askopenfilename(
            title='Select Open Gate MP4 for FFmpeg Export Commands',
            filetypes=[('Video Files', '*.MP4 *.mp4 *.MOV *.mov'), ('All Files', '*.*')]
        )
        if not src:
            return
        self._run_subprocess(
            f'FFmpeg Commands ({Path(src).name})',
            [sys.executable, str(ROOT / 'tools' / 'opengate_mp4_tool.py'), 'ffmpeg-cmd', src]
        )


if __name__ == '__main__':
    A7S3ControlApp().mainloop()
