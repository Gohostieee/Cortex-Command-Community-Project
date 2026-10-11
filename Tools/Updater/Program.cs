using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace CortexUpdater {
    static class Program {
        public static string Cache(string root) {
            string key;
            using (var sha = SHA256.Create()) key = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(root).ToLowerInvariant()))).Replace("-", "");
            string directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CortexCommand", "Updater", key);
            Directory.CreateDirectory(directory); return directory;
        }
        public static void Log(string root, string message) { File.AppendAllText(Path.Combine(Cache(root), "update.log"), DateTime.UtcNow.ToString("o") + " " + message + Environment.NewLine); }
        public static void ApplyAndLaunch(string ticketPath, int parent, bool updateOnly) {
            var ticket = UpdateCore.Json.Deserialize<Ticket>(File.ReadAllText(ticketPath));
            UpdateCore.WaitForExit(parent);
            using (var mutex = new Mutex(false, "Local\\CortexCommandUpdater-" + Path.GetFileName(Cache(ticket.Root)))) {
                try { if (!mutex.WaitOne(60000)) throw new IOException("Another update is still running."); } catch (AbandonedMutexException) { }
                try {
                    UpdateCore.Apply(ticket, delegate(int _) { }); Log(ticket.Root, "INSTALLED " + File.ReadAllText(Path.Combine(ticket.Root, "updater.json")));
                    if (!updateOnly) UpdateCore.Launch(ticket.Root, ticket.Arguments);
                } finally { mutex.ReleaseMutex(); }
            }
        }
        [STAThread]
        static int Main(string[] args) {
            UpdateCore.Initialize();
            string root = AppDomain.CurrentDomain.BaseDirectory, ticket = null; int parent = 0;
            bool checkOnly = false, updateOnly = false; var forwarded = new List<string>();
            for (int i = 0; i < args.Length; ++i) {
                if (args[i] == "--") { forwarded.AddRange(args.Skip(i + 2)); break; }
                if (args[i] == "--root") root = args[++i];
                else if (args[i] == "--parent") parent = Int32.Parse(args[++i]);
                else if (args[i] == "--apply") ticket = args[++i];
                else if (args[i] == "--check-only") checkOnly = true;
                else if (args[i] == "--update-only") updateOnly = true;
                else throw new ArgumentException("Unknown updater argument.");
            }
            root = Path.GetFullPath(root);
            try {
                if (ticket != null) { root = UpdateCore.Json.Deserialize<Ticket>(File.ReadAllText(ticket)).Root; ApplyAndLaunch(ticket, parent, updateOnly); return 0; }
                using (var mutex = new Mutex(false, "Local\\CortexCommandUpdater-" + Path.GetFileName(Cache(root)))) {
                    try { if (!mutex.WaitOne(0)) { if (!checkOnly && !updateOnly) MessageBox.Show("The game updater is already running.", "Cortex Command"); return 2; } } catch (AbandonedMutexException) { }
                    try {
                        UpdateCore.WaitForExit(parent);
                        UpdateCore.Recover(root, Cache(root));
                        var manifest = UpdateCore.Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(root, "updater.json")));
                        if ((string)manifest["repository"] != UpdateCore.Repository || Convert.ToInt32(manifest["schema"]) != 1) throw new InvalidDataException("Invalid update settings.");
                        string tag = (string)manifest["tag"];
                        if (checkOnly) { var release = UpdateCore.Check(tag); Console.WriteLine(UpdateCore.Json.Serialize(new { installed = tag, available = release == null ? null : release.Tag })); return 0; }
                        if (updateOnly) { var release = UpdateCore.Check(tag); if (release == null) return 0; Install(root, release, forwarded.ToArray(), true, delegate(int _) { }); return 0; }
                        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
                        Application.Run(new UpdaterWindow(root, tag, forwarded.ToArray())); return 0;
                    } finally { mutex.ReleaseMutex(); }
                }
            } catch (Exception error) {
                try { Log(root, "ERROR " + error.Message); } catch { }
                if (checkOnly || updateOnly) { Console.Error.WriteLine(error.Message); return 1; }
                MessageBox.Show("The game could not be updated.\n\n" + error.Message, "Cortex Command", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                if (ticket != null) {
                    try {
                        var recovery = UpdateCore.Json.Deserialize<Ticket>(File.ReadAllText(ticket));
                        var journal = UpdateCore.Json.Deserialize<Journal>(File.ReadAllText(Path.Combine(Path.GetDirectoryName(recovery.Stage), "journal.json")));
                        if (journal.Complete) UpdateCore.Launch(recovery.Root, recovery.Arguments);
                    } catch { }
                }
                return 1;
            }
        }
        public static void Install(string root, Release release, string[] arguments, bool updateOnly, Action<int> progress) {
            string transaction = Path.Combine(Cache(root), Guid.NewGuid().ToString("N")); Directory.CreateDirectory(transaction);
            string archive = Path.Combine(transaction, "release.zip");
            Log(root, "DOWNLOADING " + release.Tag);
            UpdateCore.Download(release, archive, progress);
            var ticket = UpdateCore.Stage(archive, release, root, Path.Combine(transaction, "stage"), arguments);
            string path = Path.Combine(transaction, "ticket.json"); File.WriteAllText(path, UpdateCore.Json.Serialize(ticket));
            string helper = Path.Combine(transaction, "apply.exe"); File.Copy(Assembly.GetExecutingAssembly().Location, helper);
            var start = new ProcessStartInfo(helper, "--apply " + UpdateCore.Quote(path) + " --parent " + Process.GetCurrentProcess().Id + (updateOnly ? " --update-only" : "")) { UseShellExecute = false, WorkingDirectory = root };
            Log(root, "VERIFIED " + release.Tag); Process.Start(start);
        }
    }
    sealed class UpdaterWindow : Form {
        readonly string Root, InstalledTag; readonly string[] Arguments;
        readonly Label Status = new Label(), Detail = new Label(); readonly ProgressBar Progress = new ProgressBar();
        bool Started;
        public UpdaterWindow(string root, string tag, string[] arguments) {
            Root = root; InstalledTag = tag; Arguments = arguments;
            Text = "Cortex Command"; ClientSize = new Size(490, 160); FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Segoe UI", 10); BackColor = Color.White;
            Status.Text = "Checking for updates…"; Status.Font = new Font(Font.FontFamily, 15, FontStyle.Bold); Status.SetBounds(24, 20, 440, 30);
            Detail.Text = "Installed: " + tag; Detail.SetBounds(24, 57, 440, 26);
            Progress.SetBounds(24, 96, 440, 16); Progress.Style = ProgressBarStyle.Marquee;
            Controls.AddRange(new Control[] { Status, Detail, Progress });
            Shown += async delegate { await Run(); };
            FormClosing += delegate(object sender, FormClosingEventArgs e) { if (!Started) { e.Cancel = true; } };
        }
        void StartGame() { if (Started) return; Started = true; UpdateCore.Launch(Root, Arguments); Close(); }
        async Task Run() {
            Release release;
            try { release = await Task.Run(() => UpdateCore.Check(InstalledTag)); }
            catch (Exception error) { Program.Log(Root, "CHECK FAILED " + error.Message); StartGame(); return; }
            if (release == null) { Program.Log(Root, "CURRENT " + InstalledTag); StartGame(); return; }
            Status.Text = "Updating Cortex Command…"; Detail.Text = "Downloading " + release.Tag; Progress.Style = ProgressBarStyle.Continuous;
            try {
                await Task.Run(() => Program.Install(Root, release, Arguments, false, percent => BeginInvoke((Action)(() => { Progress.Value = percent; if (percent == 100) Detail.Text = "Verifying and installing…"; }))));
                Started = true; Close();
            } catch (Exception error) {
                Program.Log(Root, "UPDATE FAILED " + error.Message);
                Status.Text = "Update could not be installed"; Detail.Text = "Your current game is unchanged."; Progress.Visible = false;
                var play = new Button { Text = "Play installed version" }; play.SetBounds(24, 98, 190, 34); play.Click += delegate { StartGame(); }; Controls.Add(play);
                var retry = new Button { Text = "Retry update" }; retry.SetBounds(230, 98, 150, 34); retry.Click += async delegate { Controls.Remove(play); Controls.Remove(retry); Progress.Visible = true; await Run(); }; Controls.Add(retry);
            }
        }
    }
}
