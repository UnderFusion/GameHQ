// The .NET 8 test host has no Playnite SDK. Only the logging surface is replaced.
namespace Playnite.SDK
{
    internal interface ILogger { void Warn(string message); }

    internal static class LogManager
    {
        private sealed class TestLog : ILogger
        {
            public void Warn(string message) => System.Diagnostics.Trace.WriteLine(message);
        }

        public static ILogger GetLogger(string name) => new TestLog();
    }
}
