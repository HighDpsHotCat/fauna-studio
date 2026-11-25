using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("Fauna Studio")]
[assembly: AssemblyProduct("Fauna Studio")]
[assembly: AssemblyDescription("동물 종 분류 및 3D 모델링")]
[assembly: AssemblyVersion("1.0.0.0")]

internal static class Launcher
{
    private static readonly string Root = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "FaunaStudio");
    private static readonly string Installed = Path.Combine(Root, Release.Id);

    private static bool IsInstalled()
    {
        return File.Exists(Path.Combine(Installed, "ready")) &&
               File.Exists(Path.Combine(Installed, "runtime", "pythonw.exe")) &&
               File.Exists(Path.Combine(Installed, "app", "app.py"));
    }

    private static void OpenStudio()
    {
        var app = Path.Combine(Installed, "app");
        var start = new ProcessStartInfo(Path.Combine(Installed, "runtime", "pythonw.exe"),
            "-E -s -X utf8 \"" + Path.Combine(app, "app.py") + "\"");
        start.WorkingDirectory = app;
        start.UseShellExecute = false;
        start.EnvironmentVariables.Remove("PYTHONHOME");
        start.EnvironmentVariables.Remove("PYTHONPATH");
        start.EnvironmentVariables.Remove("__PYVENV_LAUNCHER__");
        using (Process process = Process.Start(start)) { }
    }

    private static void AddShortcut()
    {
        var shellType = Type.GetTypeFromProgID("WScript.Shell");
        if (shellType == null) return;
        dynamic shell = Activator.CreateInstance(shellType);
        string menu = Environment.GetFolderPath(Environment.SpecialFolder.Programs);
        dynamic link = shell.CreateShortcut(Path.Combine(menu, "Fauna Studio.lnk"));
        link.TargetPath = Path.Combine(Installed, "FaunaStudio.exe");
        link.WorkingDirectory = Installed;
        link.Save();
    }

    private static void Install(BackgroundWorker worker)
    {
        Directory.CreateDirectory(Root);
        using (var mutex = new Mutex(false, @"Local\FaunaStudio-" + Release.Id))
        {
            bool acquired = false;
            try
            {
                while (!acquired)
                {
                    if (worker.CancellationPending) throw new OperationCanceledException();
                    try { acquired = mutex.WaitOne(250); }
                    catch (AbandonedMutexException) { acquired = true; }
                }
                if (IsInstalled()) return;
                string stage = Path.Combine(Root, Release.Id + ".installing");
                Directory.CreateDirectory(stage);
                try
                {
                    Extract(stage, worker);
                    if (worker.CancellationPending) throw new OperationCanceledException();
                    File.WriteAllText(Path.Combine(stage, "ready"), Release.Id);
                    if (Directory.Exists(Installed))
                        throw new IOException("기존 설치 폴더를 확인해 주세요.\n" + Installed);
                    Directory.Move(stage, Installed);
                    // Shortcut creation is optional on systems with a restricted Start menu.
                    try { AddShortcut(); } catch (System.Runtime.InteropServices.COMException) { }
                }
                finally
                {
                    if (Directory.Exists(stage)) Directory.Delete(stage, true);
                }
            }
            finally { if (acquired) mutex.ReleaseMutex(); }
        }
    }

    private static void Extract(string stage, BackgroundWorker worker)
    {
        string self = Assembly.GetExecutingAssembly().Location;
        using (var file = File.OpenRead(self))
        using (var zip = new ZipArchive(file, ZipArchiveMode.Read))
        using (var hash = SHA256.Create())
        {
            var digest = zip.GetEntry("payload.sha256");
            if (digest == null) throw new InvalidDataException("배포용 EXE를 다시 실행해 주세요.");
            string expected;
            using (var reader = new StreamReader(digest.Open())) expected = reader.ReadToEnd().Trim();
            long total = 0, copied = 0;
            foreach (var entry in zip.Entries) if (entry != digest) total += entry.Length;
            if (new DriveInfo(Path.GetPathRoot(stage)).AvailableFreeSpace < total)
                throw new IOException(String.Format("설치에 필요한 여유 공간은 {0:F1} GB입니다.", total / 1e9));
            var clock = Stopwatch.StartNew();
            var buffer = new byte[256 * 1024];
            string prefix = Path.GetFullPath(stage) + Path.DirectorySeparatorChar;
            foreach (var entry in zip.Entries)
            {
                if (entry == digest) continue;
                string destination = Path.GetFullPath(Path.Combine(stage, entry.FullName));
                if (!destination.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("패키지 경로 오류");
                Directory.CreateDirectory(Path.GetDirectoryName(destination));
                byte[] name = Encoding.UTF8.GetBytes(entry.FullName + "\0");
                hash.TransformBlock(name, 0, name.Length, name, 0);
                using (var input = entry.Open())
                using (var output = File.Create(destination))
                {
                    int count;
                    while ((count = input.Read(buffer, 0, buffer.Length)) != 0)
                    {
                        if (worker.CancellationPending) throw new OperationCanceledException();
                        output.Write(buffer, 0, count);
                        hash.TransformBlock(buffer, 0, count, buffer, 0);
                        copied += count;
                        if (clock.ElapsedMilliseconds >= 180)
                        {
                            worker.ReportProgress((int)(copied * 100 / total),
                                String.Format("실행 환경 설치 중   {0:F1} / {1:F1} GB", copied / 1e9, total / 1e9));
                            clock.Restart();
                        }
                    }
                }
            }
            hash.TransformFinalBlock(new byte[0], 0, 0);
            if (!String.Equals(BitConverter.ToString(hash.Hash).Replace("-", ""), expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("패키지 무결성을 확인할 수 없습니다. EXE 파일을 다시 복사해 주세요.");
        }
    }

    [STAThread]
    private static int Main()
    {
        AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling", false);
        AppContext.SetSwitch("Switch.System.IO.BlockLongPaths", false);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        try
        {
            if (IsInstalled()) { OpenStudio(); return 0; }
            using (var form = new Form())
            using (var worker = new BackgroundWorker())
            {
                form.Text = "Fauna Studio 설치";
                form.ClientSize = new Size(426, 145);
                form.FormBorderStyle = FormBorderStyle.FixedDialog;
                form.MaximizeBox = false;
                form.StartPosition = FormStartPosition.CenterScreen;
                var title = new Label { Text = "Fauna Studio", Left = 20, Top = 14, Width = 384, Height = 29,
                    Font = new Font("Georgia", 16, FontStyle.Bold), ForeColor = Color.FromArgb(29, 59, 49) };
                var status = new Label { Text = "설치 파일 확인 중…", Left = 20, Top = 51, Width = 384, Height = 23 };
                var progress = new ProgressBar { Left = 20, Top = 78, Width = 386, Height = 16 };
                var cancel = new Button { Text = "취소", Left = 328, Top = 105, Width = 78, Height = 27 };
                form.Controls.AddRange(new Control[] { title, status, progress, cancel });
                worker.WorkerReportsProgress = worker.WorkerSupportsCancellation = true;
                worker.DoWork += delegate(object sender, DoWorkEventArgs e) {
                    try { Install(worker); } catch (OperationCanceledException) { e.Cancel = true; }
                };
                worker.ProgressChanged += delegate(object sender, ProgressChangedEventArgs e) {
                    progress.Value = e.ProgressPercentage;
                    status.Text = (string)e.UserState;
                };
                Exception error = null;
                bool done = false, cancelled = false;
                worker.RunWorkerCompleted += delegate(object sender, RunWorkerCompletedEventArgs e) {
                    error = e.Error; cancelled = e.Cancelled; done = true; form.Close();
                };
                cancel.Click += delegate { form.Close(); };
                form.FormClosing += delegate(object sender, FormClosingEventArgs e) {
                    if (done) return;
                    e.Cancel = true; cancel.Enabled = false; status.Text = "설치 취소 중…"; worker.CancelAsync();
                };
                form.Shown += delegate { worker.RunWorkerAsync(); };
                Application.Run(form);
                if (error != null) throw error;
                if (!cancelled) OpenStudio();
            }
            return 0;
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, "Fauna Studio", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
