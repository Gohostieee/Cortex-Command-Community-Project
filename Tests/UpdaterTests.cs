using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using CortexUpdater;

class UpdaterTests {
    static int Count;
    static readonly string DeepAsset = "Data/" + new string('a', 64) + "/" + new string('b', 64) + "/" + new string('c', 64) + "/sprite.bmp";
    static void Check(bool result, string name) { if (!result) throw new Exception(name); ++Count; Console.WriteLine("PASS: " + name); }
    static void Reject(Action action, string name) { bool rejected = false; try { action(); } catch (InvalidDataException) { rejected = true; } Check(rejected, name); }
    static void Entry(ZipArchive archive, string name, string value) { using (var output = new StreamWriter(archive.CreateEntry("CortexCommand-Multiplayer/" + name).Open())) output.Write(value); }
    static Release Package(string directory, string extra) {
        string path = Path.Combine(directory, "release.zip");
        var files = new Dictionary<string, string> {
            { "Cortex Command.exe", "new-game" }, { "CortexUpdater.exe", "new-updater" },
            { "updater.json", UpdateCore.Json.Serialize(new { schema = 1, repository = UpdateCore.Repository, tag = "mp-2026.10.11.1" }) },
            { "SOURCE.tar.gz", "matching-source" }, { "LICENSE", "license" }, { "Data/version.txt", "new-data" }, { DeepAsset, "long-path-data" }
        };
        using (var zip = ZipFile.Open(path, ZipArchiveMode.Create)) {
            foreach (var item in files) Entry(zip, item.Key, item.Value);
            Entry(zip, "update-files.json", UpdateCore.Json.Serialize(files.Keys.ToArray()));
            Entry(zip, "Userdata/save.dat", "overwrite-save"); Entry(zip, "Mods/Personal.rte/file.ini", "overwrite-mod"); Entry(zip, "ScreenShots/keep.png", "overwrite-shot");
            if (extra != null) Entry(zip, extra, "unsafe");
        }
        return new Release { Tag = "mp-2026.10.11.1", Bytes = new FileInfo(path).Length, Sha256 = UpdateCore.Hash(path) };
    }
    static void Write(string root, string path, string value) { string destination = Path.Combine(root, path.Replace('/', Path.DirectorySeparatorChar)); Directory.CreateDirectory(Path.GetDirectoryName(destination)); File.WriteAllText(destination, value); }
    static void Personal(string root) { Write(root, "Userdata/save.dat", "my-save"); Write(root, "Userdata/Settings.ini", "my-settings"); Write(root, "Mods/Personal.rte/file.ini", "my-mod"); Write(root, "ScreenShots/keep.png", "my-shot"); }
    static void Preserved(string root) { Check(File.ReadAllText(Path.Combine(root, "Userdata/save.dat")) == "my-save" && File.ReadAllText(Path.Combine(root, "Userdata/Settings.ini")) == "my-settings" && File.ReadAllText(Path.Combine(root, "Mods/Personal.rte/file.ini")) == "my-mod" && File.ReadAllText(Path.Combine(root, "ScreenShots/keep.png")) == "my-shot", "personal data survives update"); }
    static void Main(string[] args) {
        UpdateCore.Initialize();
        string task = Path.GetFullPath(args[0]); Directory.CreateDirectory(task);
        string digest = new string('a', 64);
        Func<string, bool, string, object> release = (tag, draft, sha) => new {
            tag_name = tag, draft = draft, prerelease = true,
            assets = new[] { new { name = "CortexCommand-Multiplayer-" + tag + "-Windows-x64.zip", digest = sha, size = 20, browser_download_url = "https://github.com/" + UpdateCore.Repository + "/releases/download/" + tag + "/CortexCommand-Multiplayer-" + tag + "-Windows-x64.zip" } }
        };
        string listing = UpdateCore.Json.Serialize(new[] { release("mp-2026.10.11.2", true, "sha256:" + digest), release("mp-2026.10.11.1", false, "sha256:" + digest), release("mp-2026.10.04.1", false, "sha256:" + digest) });
        Check(UpdateCore.SelectRelease(listing, "mp-2026.10.11").Tag == "mp-2026.10.11.1", "newest published multiplayer prerelease is selected; draft is skipped");
        Check(UpdateCore.SelectRelease(listing, "mp-2026.10.11.1") == null, "current build is not reinstalled or downgraded");
        Check(UpdateCore.SelectRelease(UpdateCore.Json.Serialize(new[] { release("mp-2026.10.11.1", false, null) }), "mp-2026.10.11") == null, "release without published checksum is skipped");
        foreach (string unsafePath in new[] { "../escape", "Data/../../escape", "C:/escape", "/escape", "Data\\escape", "Data/NUL", "Data/file:stream", "Data/alias." }) Reject(() => UpdateCore.Destination(task, unsafePath), "unsafe path rejected: " + unsafePath);
        string install = Path.Combine(task, "install"), transaction = Path.Combine(task, "transaction"); Directory.CreateDirectory(install); Directory.CreateDirectory(transaction);
        Write(install, "Cortex Command.exe", "old-game"); Write(install, "Data/obsolete.txt", "old-data"); Write(install, "update-files.json", "[\"Cortex Command.exe\",\"Data/obsolete.txt\"]"); Personal(install);
        string oldDeep = UpdateCore.Destination(install, DeepAsset);
        Directory.CreateDirectory(Path.GetDirectoryName(oldDeep)); File.WriteAllText(oldDeep, "old-long-path-data");
        var package = Package(transaction, null);
        string archive = Path.Combine(transaction, "release.zip");
        var invalid = new Release { Tag = package.Tag, Bytes = package.Bytes, Sha256 = digest };
        Reject(() => UpdateCore.Stage(archive, invalid, install, Path.Combine(task, "invalid"), new string[0]), "checksum mismatch fails before installation");
        var ticket = UpdateCore.Stage(archive, package, install, Path.Combine(transaction, "stage"), new string[0]);
        UpdateCore.Apply(ticket, delegate(int _) { });
        Check(File.ReadAllText(Path.Combine(install, "Cortex Command.exe")) == "new-game" && File.ReadAllText(Path.Combine(install, "Data/version.txt")) == "new-data", "real ZIP extraction and installation replace managed game files");
        Check(File.ReadAllText(UpdateCore.Destination(install, DeepAsset)) == "long-path-data", "deep game-data paths beyond MAX_PATH install successfully");
        Check(!File.Exists(Path.Combine(install, "Data/obsolete.txt")), "obsolete managed files are removed"); Preserved(install);
        string lockedTx = Path.Combine(task, "locked-transaction"), lockedRoot = Path.Combine(task, "locked-install");
        Directory.CreateDirectory(lockedTx); Directory.CreateDirectory(lockedRoot); Write(lockedRoot, "Cortex Command.exe", "new-game");
        var lockedPackage = Package(lockedTx, null);
        var lockedTicket = UpdateCore.Stage(Path.Combine(lockedTx, "release.zip"), lockedPackage, lockedRoot, Path.Combine(lockedTx, "stage"), new string[0]);
        using (var held = File.Open(Path.Combine(lockedRoot, "Cortex Command.exe"), FileMode.Open, FileAccess.Read, FileShare.Read)) UpdateCore.Apply(lockedTicket, delegate(int _) { });
        Check(File.ReadAllText(Path.Combine(lockedRoot, "Data/version.txt")) == "new-data", "an unchanged file can remain open while other managed files update");
        string retryTx = Path.Combine(task, "retry-transaction"), retryRoot = Path.Combine(task, "retry-install");
        Directory.CreateDirectory(retryTx); Directory.CreateDirectory(retryRoot); Write(retryRoot, "Cortex Command.exe", "old-game");
        var retryPackage = Package(retryTx, null);
        var retryTicket = UpdateCore.Stage(Path.Combine(retryTx, "release.zip"), retryPackage, retryRoot, Path.Combine(retryTx, "stage"), new string[0]);
        var temporaryLock = File.Open(Path.Combine(retryRoot, "Cortex Command.exe"), FileMode.Open, FileAccess.Read, FileShare.Read);
        var unlock = System.Threading.Tasks.Task.Run(() => { System.Threading.Thread.Sleep(200); temporaryLock.Dispose(); });
        UpdateCore.Apply(retryTicket, delegate(int _) { }); unlock.Wait();
        Check(File.ReadAllText(Path.Combine(retryRoot, "Cortex Command.exe")) == "new-game", "temporary replacement locks are retried before installation fails");
        string failure = Path.Combine(task, "failure"), failedInstall = Path.Combine(task, "failed-install"); Directory.CreateDirectory(failure); Directory.CreateDirectory(failedInstall);
        Write(failedInstall, "Cortex Command.exe", "old-game"); Write(failedInstall, "Data/version.txt", "new-data"); Personal(failedInstall);
        var failedPackage = Package(failure, null);
        var failedTicket = UpdateCore.Stage(Path.Combine(failure, "release.zip"), failedPackage, failedInstall, Path.Combine(failure, "stage"), new string[0]);
        bool failed = false; try { UpdateCore.Apply(failedTicket, i => { if (i == 2) throw new IOException("Injected interrupted write"); }); } catch (IOException) { failed = true; }
        Check(failed && File.ReadAllText(Path.Combine(failedInstall, "Cortex Command.exe")) == "old-game" && !File.Exists(Path.Combine(failedInstall, "CortexUpdater.exe")), "failed installation restores old executable and removes new files"); Preserved(failedInstall);
        Check(File.ReadAllText(Path.Combine(failedInstall, "Data/version.txt")) == "new-data", "rollback preserves unchanged managed files without backups");
        string crash = Path.Combine(task, "crash-cache"), crashTx = Path.Combine(crash, "transaction"), crashRoot = Path.Combine(task, "crash-install"); Directory.CreateDirectory(crashTx); Directory.CreateDirectory(crashRoot);
        Write(crashRoot, "Cortex Command.exe", "partial-game"); Write(crashTx, "backup/Cortex Command.exe", "old-game"); Directory.CreateDirectory(Path.Combine(crashTx, "stage"));
        var interrupted = new Journal { Ticket = new Ticket { Root = crashRoot, Stage = Path.Combine(crashTx, "stage"), Files = new[] { "Cortex Command.exe" }, DeleteFiles = new string[0] }, Existing = new[] { "Cortex Command.exe" }, Complete = false };
        UpdateCore.WriteJournal(interrupted); UpdateCore.Recover(crashRoot, crash);
        Check(File.ReadAllText(Path.Combine(crashRoot, "Cortex Command.exe")) == "old-game", "next startup recovers a persisted interrupted-update journal");
        foreach (string unsafeEntry in new[] { "../escape", "Data/../../escape", "Data/file:stream", "Data/version.txt" }) {
            string bad = Path.Combine(task, "bad-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(bad); var badPackage = Package(bad, unsafeEntry);
            Reject(() => UpdateCore.Stage(Path.Combine(bad, "release.zip"), badPackage, install, Path.Combine(bad, "stage"), new string[0]), "unsafe or duplicate ZIP entry is rejected: " + unsafeEntry);
        }
        string launch = Path.Combine(task, "launch"); Directory.CreateDirectory(launch); File.Copy(args[1], Path.Combine(launch, "Cortex Command.exe"));
        string[] arguments = { "ordinary", "space arg", "quote\"arg", "trailing\\" };
        using (var process = UpdateCore.Launch(launch, arguments)) Check(process.WaitForExit(10000) && process.ExitCode == 0, "updater launches a real game process");
        var marker = UpdateCore.Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(launch, "launch-result.json")));
        Check((string)marker["sentinel"] == "1" && ((System.Collections.IEnumerable)marker["arguments"]).Cast<string>().SequenceEqual(arguments), "one-launch marker and quoted game arguments are preserved");
        string helperTx = Path.Combine(task, "helper-transaction"), helperRoot = Path.Combine(task, "helper-install");
        Directory.CreateDirectory(helperTx); Directory.CreateDirectory(helperRoot); Personal(helperRoot);
        File.Copy(args[1], Path.Combine(helperRoot, "Cortex Command.exe"));
        Write(helperRoot, "CortexUpdater.exe", "old-updater");
        var helperPackage = Package(helperTx, null);
        var helperTicket = UpdateCore.Stage(Path.Combine(helperTx, "release.zip"), helperPackage, helperRoot, Path.Combine(helperTx, "stage"), arguments);
        File.Copy(args[1], Path.Combine(helperTicket.Stage, "Cortex Command.exe"), true);
        File.Copy(args[2], Path.Combine(helperTicket.Stage, "CortexUpdater.exe"), true);
        string helperTicketPath = Path.Combine(helperTx, "ticket.json"), helperPath = Path.Combine(helperTx, "apply.exe");
        File.WriteAllText(helperTicketPath, UpdateCore.Json.Serialize(helperTicket)); File.Copy(args[2], helperPath);
        using (var process = Process.Start(new ProcessStartInfo(helperPath, "--apply " + UpdateCore.Quote(helperTicketPath)) { UseShellExecute = false, WorkingDirectory = helperRoot }))
            Check(process.WaitForExit(20000) && process.ExitCode == 0, "separate updater helper installs and exits successfully");
        Check(UpdateCore.Hash(Path.Combine(helperRoot, "CortexUpdater.exe")) == UpdateCore.Hash(args[2]), "helper replaces the updater executable itself");
        for (int i = 0; i < 100 && !File.Exists(Path.Combine(helperRoot, "launch-result.json")); ++i) System.Threading.Thread.Sleep(50);
        var helperMarker = UpdateCore.Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(helperRoot, "launch-result.json")));
        Check((string)helperMarker["sentinel"] == "1" && ((System.Collections.IEnumerable)helperMarker["arguments"]).Cast<string>().SequenceEqual(arguments), "helper restarts the updated game with the original arguments");
        Preserved(helperRoot);
        Console.WriteLine("PASS: " + Count + " updater checks.");
    }
}
