using System;
using System.IO;
using System.Web.Script.Serialization;
class MockGame {
    static void Main(string[] args) { File.WriteAllText("launch-result.json", new JavaScriptSerializer().Serialize(new { arguments = args, sentinel = Environment.GetEnvironmentVariable("CCCP_UPDATER_LAUNCH") })); }
}
