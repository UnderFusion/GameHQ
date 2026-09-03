using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
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
        private bool _launchAttempted;

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

            var exePath = GameHQLocator.Locate(Settings.ExePath);
            if (exePath != null)
                GameHQProcessLauncher.TryLaunch(exePath);
        }

        public override void OnApplicationStarted(OnApplicationStartedEventArgs args)
        {
            // One best-effort launch if GameHQ isn't reachable yet; the
            // client's own background loop keeps retrying the connection
            // regardless, so a failed launch here is never fatal.
            if (_launchAttempted || !Settings.StartWithPlaynite || Client.State == IntegrationConnectionState.Connected) return;
            _launchAttempted = true;

            var exePath = GameHQLocator.Locate(Settings.ExePath);
            if (exePath != null)
                GameHQProcessLauncher.TryLaunch(exePath);
        }

        public override void OnApplicationStopped(OnApplicationStoppedEventArgs args)
        {
            // Never close GameHQ here — it may be tray-resident, exporting a
            // clip, or used standalone without Playnite at all.
            _lifecycle.ApplicationStopping();
            Client.Stop();
        }

        public override void OnGameStarting(OnGameStartingEventArgs args)
        {
            if (Settings.StartOnGameLaunchIfNotRunning && Client.State != IntegrationConnectionState.Connected)
            {
                var exePath = GameHQLocator.Locate(Settings.ExePath);
                if (exePath != null)
                    GameHQProcessLauncher.TryLaunch(exePath);
            }

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
