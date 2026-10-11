using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace CortexUpdater {
    public sealed class Release {
        public string Tag, Url, Sha256;
        public long Bytes;
    }
    public sealed class Ticket {
        public string Root, Stage;
        public string[] Arguments, Files, DeleteFiles;
    }
    public sealed class Journal {
        public Ticket Ticket;
        public string[] Existing;
        public bool Complete;
    }
    public static class UpdateCore {
        public const string Repository = "Gohostieee/Cortex-Command-Community-Project";
        public static readonly JavaScriptSerializer Json;
        static UpdateCore() {
            // Enable modern .NET 4.8 path handling before any filesystem API
            // caches its compatibility defaults. Game assets can exceed MAX_PATH.
            AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling", false);
            AppContext.SetSwitch("Switch.System.IO.BlockLongPaths", false);
            Json = new JavaScriptSerializer { MaxJsonLength = 4 * 1024 * 1024 };
        }
        public static void Initialize() { }
        public static string Hash(string path) { using (var stream = File.OpenRead(path)) using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant(); }
        public static Version TagVersion(string tag) {
            Version version;
            if (tag == null || !Regex.IsMatch(tag, @"^mp-\d+\.\d+\.\d+(\.\d+)?$") || !Version.TryParse(tag.Substring(3), out version)) return null;
            return new Version(version.Major, version.Minor, version.Build, Math.Max(0, version.Revision));
        }
        public static Release SelectRelease(string json, string current) {
            var installed = TagVersion(current);
            if (installed == null) throw new InvalidDataException("The installed release version is invalid.");
            Release newest = null; Version best = installed;
            foreach (var item in (object[])Json.DeserializeObject(json)) {
                var release = (Dictionary<string, object>)item;
                if ((bool)release["draft"]) continue;
                string tag = (string)release["tag_name"]; var version = TagVersion(tag);
                if (version == null || version <= best) continue;
                string name = "CortexCommand-Multiplayer-" + tag + "-Windows-x64.zip";
                foreach (var raw in (object[])release["assets"]) {
                    var asset = (Dictionary<string, object>)raw;
                    if ((string)asset["name"] != name || !asset.ContainsKey("digest")) continue;
                    string digest = asset["digest"] as string;
                    if (digest == null || !Regex.IsMatch(digest, "^sha256:[a-f0-9]{64}$")) continue;
                    var url = (string)asset["browser_download_url"];
                    var uri = new Uri(url);
                    if (uri.Scheme != "https" || uri.Host != "github.com" || uri.AbsolutePath != "/" + Repository + "/releases/download/" + tag + "/" + name) continue;
                    long size = Convert.ToInt64(asset["size"]);
                    if (size <= 0 || size > 2L * 1024 * 1024 * 1024) continue;
                    newest = new Release { Tag = tag, Url = url, Sha256 = digest.Substring(7), Bytes = size }; best = version;
                }
            }
            return newest;
        }
        static HttpWebRequest Request(string url, int timeout) {
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.UserAgent = "CortexCommandUpdater/1.0";
            request.Accept = "application/vnd.github+json";
            request.Timeout = timeout; request.ReadWriteTimeout = timeout;
            return request;
        }
        public static Release Check(string current) {
            var request = Request("https://api.github.com/repos/" + Repository + "/releases?per_page=100", 7000);
            using (var response = request.GetResponse()) using (var reader = new StreamReader(response.GetResponseStream())) {
                string text = reader.ReadToEnd();
                if (text.Length > 4 * 1024 * 1024) throw new InvalidDataException("Release listing is too large.");
                return SelectRelease(text, current);
            }
        }
        public static void Download(Release release, string archive, Action<int> progress) {
            var request = Request(release.Url, 30000);
            using (var response = request.GetResponse()) using (var input = response.GetResponseStream()) using (var output = new FileStream(archive, FileMode.CreateNew)) {
                var buffer = new byte[128 * 1024]; long total = 0; int count, lastProgress = -1;
                while ((count = input.Read(buffer, 0, buffer.Length)) > 0) {
                    total += count;
                    if (total > release.Bytes) throw new InvalidDataException("Download exceeds its published size.");
                    output.Write(buffer, 0, count); int percent = (int)(total * 100 / release.Bytes);
                    if (percent != lastProgress) { progress(percent); lastProgress = percent; }
                }
                if (total != release.Bytes) throw new InvalidDataException("Download is incomplete.");
            }
            if (Hash(archive) != release.Sha256) throw new InvalidDataException("Download checksum differs from the published release.");
        }
        static bool Protected(string relative) {
            var first = relative.Split('/')[0];
            return new[] { "Userdata", "Mods", "ScreenShots", "Settings.ini" }.Contains(first, StringComparer.OrdinalIgnoreCase);
        }
        static string Extended(string path) {
            if (path.StartsWith(@"\\?\", StringComparison.Ordinal)) return path;
            return path.StartsWith(@"\\", StringComparison.Ordinal) ? @"\\?\UNC\" + path.Substring(2) : @"\\?\" + path;
        }
        public static string Destination(string root, string relative) {
            if (String.IsNullOrEmpty(relative) || relative.Contains("\\") || relative.Contains(":") || relative.StartsWith("/") || relative.Split('/').Any(p => p == ".." || p == "." || p.EndsWith(" ") || p.EndsWith(".") || Regex.IsMatch(p, @"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)", RegexOptions.IgnoreCase))) throw new InvalidDataException("Unsafe package path.");
            var fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            var destination = Path.GetFullPath(Path.Combine(fullRoot, relative.Replace('/', Path.DirectorySeparatorChar)));
            if (!destination.StartsWith(fullRoot, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Package escapes its installation folder.");
            fullRoot = Extended(fullRoot); destination = Extended(destination);
            for (var path = destination; path != null && path.TrimEnd(Path.DirectorySeparatorChar) != fullRoot.TrimEnd(Path.DirectorySeparatorChar); path = Path.GetDirectoryName(path))
                if ((File.Exists(path) || Directory.Exists(path)) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("Update target contains a linked file or folder.");
            return destination;
        }
        public static Ticket Stage(string archive, Release release, string root, string stage, string[] arguments) {
            if (new FileInfo(archive).Length != release.Bytes || Hash(archive) != release.Sha256) throw new InvalidDataException("Package checksum or length is invalid.");
            Directory.CreateDirectory(stage);
            var files = new HashSet<string>(StringComparer.OrdinalIgnoreCase); long expanded = 0;
            using (var zip = ZipFile.OpenRead(archive)) {
                if (zip.Entries.Count > 100000) throw new InvalidDataException("Package has too many entries.");
                foreach (var entry in zip.Entries) {
                    const string prefix = "CortexCommand-Multiplayer/";
                    if (!entry.FullName.StartsWith(prefix, StringComparison.Ordinal)) throw new InvalidDataException("Unexpected package layout.");
                    string relative = entry.FullName.Substring(prefix.Length);
                    if (relative.Length == 0) continue;
                    Destination(stage, relative.TrimEnd('/'));
                    if (((entry.ExternalAttributes >> 16) & 0xf000) == 0xa000) throw new InvalidDataException("Linked package entries are unsupported.");
                    if (entry.FullName.EndsWith("/")) continue;
                    if (Protected(relative)) continue;
                    if (!files.Add(relative)) throw new InvalidDataException("Duplicate package path.");
                    expanded += entry.Length;
                    if (expanded > 4L * 1024 * 1024 * 1024) throw new InvalidDataException("Package expands beyond its size limit.");
                    string destination = Destination(stage, relative);
                    Directory.CreateDirectory(Path.GetDirectoryName(destination));
                    using (var input = entry.Open()) using (var output = new FileStream(destination, FileMode.CreateNew)) input.CopyTo(output);
                }
            }
            foreach (string required in new[] { "Cortex Command.exe", "CortexUpdater.exe", "updater.json", "update-files.json", "SOURCE.tar.gz", "LICENSE" }) if (!files.Contains(required)) throw new InvalidDataException("Release lacks " + required);
            var manifest = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(stage, "updater.json")));
            if ((string)manifest["tag"] != release.Tag || (string)manifest["repository"] != Repository || Convert.ToInt32(manifest["schema"]) != 1) throw new InvalidDataException("Release metadata does not match its package.");
            var managed = Json.Deserialize<string[]>(File.ReadAllText(Path.Combine(stage, "update-files.json")));
            var managedSet = new HashSet<string>(managed, StringComparer.OrdinalIgnoreCase);
            if (managedSet.Count != managed.Length || !managedSet.SetEquals(files.Where(p => p != "update-files.json"))) throw new InvalidDataException("Managed-file list differs from the release contents.");
            string previous = Path.Combine(root, "update-files.json");
            var oldFiles = File.Exists(previous) ? Json.Deserialize<string[]>(File.ReadAllText(previous)) : new string[0];
            foreach (var file in oldFiles) { Destination(root, file); if (Protected(file)) throw new InvalidDataException("Managed-file list includes personal data."); }
            return new Ticket { Root = Path.GetFullPath(root), Stage = Path.GetFullPath(stage), Arguments = arguments, Files = files.OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToArray(), DeleteFiles = oldFiles.Where(p => !files.Contains(p)).Distinct(StringComparer.OrdinalIgnoreCase).ToArray() };
        }
        static void Replace(string source, string destination) {
            Directory.CreateDirectory(Path.GetDirectoryName(destination));
            string temporary = destination + ".cc-update-" + Guid.NewGuid().ToString("N");
            try {
                File.Copy(source, temporary, false);
                if (File.Exists(destination)) File.Replace(temporary, destination, null); else File.Move(temporary, destination);
            } finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
        public static void Apply(Ticket ticket, Action<int> beforeWrite) {
            string backup = Path.Combine(Path.GetDirectoryName(ticket.Stage), "backup"); Directory.CreateDirectory(backup);
            var existing = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var affected = ticket.Files.Concat(ticket.DeleteFiles ?? new string[0]).ToArray();
            foreach (string file in affected) {
                if (Protected(file)) throw new InvalidDataException("Update attempts to replace personal files.");
                string destination = Destination(ticket.Root, file);
                if (File.Exists(destination)) { string saved = Destination(backup, file); Directory.CreateDirectory(Path.GetDirectoryName(saved)); File.Copy(destination, saved, false); existing.Add(file); }
            }
            var journal = new Journal { Ticket = ticket, Existing = existing.ToArray(), Complete = false };
            WriteJournal(journal);
            try {
                for (int i = 0; i < ticket.Files.Length; ++i) {
                    string file = ticket.Files[i]; beforeWrite(i);
                    Replace(Destination(ticket.Stage, file), Destination(ticket.Root, file));
                }
                foreach (string file in ticket.DeleteFiles ?? new string[0]) { var destination = Destination(ticket.Root, file); if (File.Exists(destination)) File.Delete(destination); }
                journal.Complete = true; WriteJournal(journal);
            } catch {
                Rollback(journal);
                throw;
            }
        }
        public static void WriteJournal(Journal journal) {
            string path = Path.Combine(Path.GetDirectoryName(journal.Ticket.Stage), "journal.json");
            string temporary = path + ".tmp";
            File.WriteAllText(temporary, Json.Serialize(journal));
            if (File.Exists(path)) File.Replace(temporary, path, null); else File.Move(temporary, path);
        }
        public static void Rollback(Journal journal) {
            string backup = Path.Combine(Path.GetDirectoryName(journal.Ticket.Stage), "backup");
            var existing = new HashSet<string>(journal.Existing, StringComparer.OrdinalIgnoreCase);
            foreach (string file in journal.Ticket.Files.Concat(journal.Ticket.DeleteFiles ?? new string[0]).Reverse()) {
                if (Protected(file)) throw new InvalidDataException("Recovery attempts to replace personal data.");
                string destination = Destination(journal.Ticket.Root, file);
                if (existing.Contains(file)) {
                    string saved = Destination(backup, file);
                    if (!File.Exists(destination) || Hash(destination) != Hash(saved)) Replace(saved, destination);
                } else if (File.Exists(destination)) File.Delete(destination);
            }
            journal.Complete = true; WriteJournal(journal);
        }
        public static void Recover(string root, string cache) {
            string boundary = Path.GetFullPath(cache).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            foreach (var directory in Directory.GetDirectories(cache)) {
                if (!Path.GetFullPath(directory).StartsWith(boundary, StringComparison.OrdinalIgnoreCase) || (File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0) throw new IOException("Invalid updater cache folder.");
                string path = Path.Combine(directory, "journal.json");
                if (!File.Exists(path)) continue;
                var journal = Json.Deserialize<Journal>(File.ReadAllText(path));
                if (!String.Equals(Path.GetFullPath(journal.Ticket.Root), Path.GetFullPath(root), StringComparison.OrdinalIgnoreCase) || Path.GetFullPath(journal.Ticket.Stage) != Path.Combine(directory, "stage")) throw new InvalidDataException("Recovery belongs to a different installation.");
                if (!journal.Complete) Rollback(journal);
                try { Directory.Delete(Extended(directory), true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
        }
        public static string Quote(string argument) {
            var value = new StringBuilder("\""); int slashes = 0;
            foreach (char c in argument) {
                if (c == '\\') { ++slashes; continue; }
                if (c == '"') value.Append('\\', slashes * 2 + 1).Append(c); else value.Append('\\', slashes).Append(c);
                slashes = 0;
            }
            return value.Append('\\', slashes * 2).Append('"').ToString();
        }
        public static void WaitForExit(int pid) { if (pid <= 0) return; try { using (var process = Process.GetProcessById(pid)) if (!process.WaitForExit(60000)) throw new IOException("Close the game before updating it."); } catch (ArgumentException) { } }
        public static Process Launch(string root, string[] arguments) {
            var start = new ProcessStartInfo(Path.Combine(root, "Cortex Command.exe"), String.Join(" ", arguments.Select(Quote))) { WorkingDirectory = root, UseShellExecute = false };
            start.EnvironmentVariables["CCCP_UPDATER_LAUNCH"] = "1";
            return Process.Start(start);
        }
    }
}
