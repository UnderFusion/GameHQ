using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading.Tasks;
using GameHQ.Playnite.Localization;
using GameHQ.Playnite.Protocol;
using GameHQ.Playnite.Settings;
using Playnite.SDK;
using Playnite.SDK.Events;
using Playnite.SDK.Plugins;

namespace GameHQ.Playnite
{
    // Entry point Playnite loads via extension.yaml. The pipe client keeps
    // trying to connect/reconnect for the plugin's whole lifetime.
    public class GameHQPlugin : GenericPlugin
    {
        // Stable per p1-1's reserved identifiers; must never change once published.
        public override Guid Id { get; } = new Guid("6f2b6a0a-6e0a-4b8e-9b7b-3a7d7c8b6a1d");

        internal IntegrationClient Client { get; }
        internal GameHQIntegrationSettings Settings { get; private set; }
        internal PluginLocalization Strings { get; }

        private readonly GameHQIntegrationSettingsViewModel _settingsViewModel;
        private readonly GameLifecycleForwarder _lifecycle;
        private readonly GameHQStartupSupervisor _startup;
        private static readonly ILogger Logger = LogManager.GetLogger(nameof(GameHQPlugin));

        public GameHQPlugin(IPlayniteAPI api) : base(api)
        {
            Properties = new GenericPluginProperties { HasSettings = true };
            Settings = LoadPluginSettings<GameHQIntegrationSettings>() ?? new GameHQIntegrationSettings();

            var assembly = Assembly.GetExecutingAssembly();
            var version = assembly.GetName().Version.ToString();
            var pluginDirectory = Path.GetDirectoryName(assembly.Location);
            Strings = new PluginLocalization(
                ResourceProvider.GetString,
                Path.Combine(pluginDirectory, "Localization", "en_US.xaml"));
            Client = new IntegrationClient(version);
            _lifecycle = new GameLifecycleForwarder(api, Client);
            _settingsViewModel = new GameHQIntegrationSettingsViewModel(this, api);
            _startup = new GameHQStartupSupervisor(
                () => Client.State == IntegrationConnectionState.Connected,
                IsGameHQRunning,
                LaunchGameHQ,
                (delay, action) => Task.Delay(delay).ContinueWith(_ => action()),
                message => Logger.Info(message),
                message => Logger.Warn(message));
            Client.Start();
        }

        internal void SaveSettings(GameHQIntegrationSettings settings)
        {
            Settings = settings;
            SavePluginSettings(settings);
        }

        // Focuses a running GameHQ, or launches it if not reachable. Used by
        // both the settings page and the "Open GameHQ" main-menu command.
        internal void OpenOrLaunchGameHQ()
        {
            if (Client.State == IntegrationConnectionState.Connected)
            {
                Client.Send(new IntegrationMessage("app.activate").Set("requestId", Guid.NewGuid().ToString("N")));
                return;
            }

            LaunchGameHQ();
        }

        private bool LaunchGameHQ()
        {
            var exePath = GameHQLocator.Locate(Settings.ExePath);
            return exePath != null && GameHQProcessLauncher.TryLaunch(exePath);
        }

        // The root launcher and app\GameHQ.exe share this process name.
        private static bool IsGameHQRunning()
        {
            var processes = Process.GetProcessesByName("GameHQ");
            foreach (var process in processes) process.Dispose();
            return processes.Length > 0;
        }

        public override void OnApplicationStarted(OnApplicationStartedEventArgs args)
        {
            // Bounded launch-and-verify: Xbox mode can defer GameHQ's own
            // autostart, and Playnite may be the only thing started at boot.
            if (Settings.StartWithPlaynite)
                _startup.Ensure("Playnite started");
        }

        public override void OnApplicationStopped(OnApplicationStoppedEventArgs args)
        {
            // Never close GameHQ here — it may be tray-resident, exporting a
            // clip, or used standalone without Playnite at all.
            _startup.Stop();
            _lifecycle.ApplicationStopping();
            Client.Stop();
        }

        public override void OnGameStarting(OnGameStartingEventArgs args)
        {
            if (Settings.StartOnGameLaunchIfNotRunning)
                _startup.Ensure("game starting: " + args.Game.Name);

            _lifecycle.GameStarting(args.Game);
        }

        public override void OnGameStarted(OnGameStartedEventArgs args)
        {
            _lifecycle.GameStarted(args.Game, args.StartedProcessId);
        }

        public override void OnGameStopped(OnGameStoppedEventArgs args)
        {
            _lifecycle.GameStopped(args.Game);
        }

        public override void OnGameStartupCancelled(OnGameStartupCancelledEventArgs args)
        {
            _lifecycle.GameStartupCancelled(args.Game);
        }

        public override IEnumerable<MainMenuItem> GetMainMenuItems(GetMainMenuItemsArgs args)
        {
            yield return new MainMenuItem
            {
                Description = Strings.Get("LOCGameHQIntegrationOpenGameHQ"),
                MenuSection = "@" + Strings.Get("LOCGameHQIntegrationName"),
                Action = _ => OpenOrLaunchGameHQ()
            };
        }

        public override ISettings GetSettings(bool firstRunSettings)
        {
            return _settingsViewModel;
        }

        public override System.Windows.Controls.UserControl GetSettingsView(bool firstRunView)
        {
            return new GameHQIntegrationSettingsView();
        }
    }
}
