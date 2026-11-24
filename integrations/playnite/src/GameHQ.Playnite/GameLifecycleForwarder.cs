using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using GameHQ.Playnite.Protocol;
using Playnite.SDK;
using Playnite.SDK.Models;

namespace GameHQ.Playnite
{
    // Tracks active Playnite game sessions and turns Playnite's plugin
    // callbacks into GameHQ.Local.v1 lifecycle messages. See plan item p5-3
    // and docs/integration-protocol.md for the wire contract. Every method
    // here only ever queues an outgoing message (IntegrationClient.Send
    // never blocks) — it must never slow down a Playnite game launch.
    internal sealed class GameLifecycleForwarder
    {
        private const int MaxSyncedGames = 64;

        private readonly IPlayniteAPI _api;
        private readonly IntegrationClient _client;
        private readonly ConcurrentDictionary<Guid, Session> _sessions = new ConcurrentDictionary<Guid, Session>();

        public GameLifecycleForwarder(IPlayniteAPI api, IntegrationClient client)
        {
            _api = api;
            _client = client;
            _client.StateChanged += OnClientStateChanged;
        }

        public void ApplicationStopping()
        {
            _client.Send(new IntegrationMessage("playnite.application.stopping")
                .Set("occurredAtUtc", UtcNowIso()));
        }

        public void GameStarting(Game game)
        {
            var session = new Session(Guid.NewGuid(), game, _api);
            _sessions[game.Id] = session;
            _client.Send(BuildGameMessage("playnite.game.starting", session));
        }

        public void GameStarted(Game game, int? startedProcessId, GameAction sourceAction)
        {
            var session = _sessions.GetOrAdd(game.Id, _ => new Session(Guid.NewGuid(), game, _api));
            // Folder tracking can report no PID. Send only the selected file
            // action. GameHQ verifies its absolute path and installation root.
            var executable = sourceAction?.Type == GameActionType.File
                ? ExpandOptionalPath(_api, game, sourceAction.Path) : null;
            lock (session)
            {
                session.StartedProcessId = startedProcessId > 0 ? startedProcessId : null;
                session.LaunchExecutable = executable?.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) == true
                    ? executable : null;
            }
            _client.Send(BuildGameMessage("playnite.game.started", session));
        }

        public void GameStopped(Game game)
        {
            Session session;
            if (_sessions.TryRemove(game.Id, out session))
                _client.Send(BuildGameMessage("playnite.game.stopped", session));
        }

        public void GameStartupCancelled(Game game)
        {
            Session session;
            if (_sessions.TryRemove(game.Id, out session))
                _client.Send(BuildGameMessage("playnite.game.startup_cancelled", session));
        }

        // Fired on every successful handshake, including reconnects, so a
        // missed disconnect never leaves a phantom "game running" state on
        // the app side — the snapshot REPLACES, it does not merge.
        private void OnClientStateChanged(IntegrationConnectionState state)
        {
            if (state != IntegrationConnectionState.Connected) return;

            _client.Send(new IntegrationMessage("playnite.application.started")
                .Set("occurredAtUtc", UtcNowIso()));

            var games = _sessions.Values.Take(MaxSyncedGames)
                .Select(s => (object)BuildGameFields(s))
                .ToList();

            _client.Send(new IntegrationMessage("playnite.state.sync").Set("games", games));
        }

        private IntegrationMessage BuildGameMessage(string type, Session session)
        {
            var message = new IntegrationMessage(type).Set("occurredAtUtc", UtcNowIso());
            foreach (var field in BuildGameFields(session))
                message.Set(field.Key, field.Value);
            return message;
        }

        private static string UtcNowIso()
        {
            return DateTime.UtcNow.ToString("o");
        }

        private static string ExpandOptionalPath(IPlayniteAPI api, Game game, string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return null;
            try
            {
                var expanded = api.ExpandGameVariables(game, path);
                return expanded != null && expanded.Length <= 4096 ? expanded : null;
            }
            catch (Exception)
            {
                // Optional library metadata must never drop a lifecycle event.
                return null;
            }
        }

        private static Dictionary<string, object> BuildGameFields(Session session)
        {
            var fields = new Dictionary<string, object>
            {
                { "sessionId", session.SessionId.ToString() },
                { "playniteGameId", session.PlayniteGameId.ToString() },
                { "name", session.Name },
            };

            if (!string.IsNullOrEmpty(session.SourceName)) fields["sourceName"] = session.SourceName;
            if (session.PlatformNames.Count > 0) fields["platformNames"] = session.PlatformNames;
            if (!string.IsNullOrEmpty(session.InstallDirectory)) fields["installDirectory"] = session.InstallDirectory;
            if (!string.IsNullOrEmpty(session.SelectedRomFile)) fields["selectedRomFile"] = session.SelectedRomFile;
            // A reconnect snapshot can run concurrently with GameStarted.
            lock (session)
            {
                if (!string.IsNullOrEmpty(session.LaunchExecutable)) fields["launchExecutable"] = session.LaunchExecutable;
                if (session.StartedProcessId.HasValue) fields["startedProcessId"] = session.StartedProcessId.Value;
            }

            return fields;
        }

        private sealed class Session
        {
            public Guid SessionId { get; private set; }
            public Guid PlayniteGameId { get; private set; }
            public string Name { get; private set; }
            public string SourceName { get; private set; }
            public List<string> PlatformNames { get; private set; }
            public string InstallDirectory { get; private set; }
            public string LaunchExecutable { get; set; }
            public string SelectedRomFile { get; private set; }
            public int? StartedProcessId { get; set; }

            public Session(Guid sessionId, Game game, IPlayniteAPI api)
            {
                SessionId = sessionId;
                PlayniteGameId = game.Id;
                Name = game.Name;
                InstallDirectory = ExpandOptionalPath(api, game, game.InstallDirectory);
                SourceName = ResolveSourceName(game, api);
                PlatformNames = ResolvePlatformNames(game, api);
                SelectedRomFile = ResolveSelectedRomFile(game);
            }

            private static string ResolveSourceName(Game game, IPlayniteAPI api)
            {
                try
                {
                    return game.SourceId != Guid.Empty ? api.Database.Sources.Get(game.SourceId)?.Name : null;
                }
                catch (Exception)
                {
                    return null;
                }
            }

            private static List<string> ResolvePlatformNames(Game game, IPlayniteAPI api)
            {
                try
                {
                    if (game.Platforms == null) return new List<string>();
                    return game.Platforms.Select(p => p.Name).Where(n => !string.IsNullOrEmpty(n)).ToList();
                }
                catch (Exception)
                {
                    return new List<string>();
                }
            }

            private static string ResolveSelectedRomFile(Game game)
            {
                try
                {
                    return game.Roms != null && game.Roms.Count > 0 ? game.Roms[0].Path : null;
                }
                catch (Exception)
                {
                    return null;
                }
            }
        }
    }
}
