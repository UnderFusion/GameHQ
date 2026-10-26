using System;
using System.Collections.Generic;
using Xunit;

namespace GameHQ.Playnite.Tests
{
    public class GameHQStartupSupervisorTests
    {
        private sealed class Harness
        {
            public bool Connected;
            public bool Running;
            public int Launches;
            public readonly Queue<Action> Pending = new Queue<Action>();
            public readonly List<string> Warnings = new List<string>();
            public readonly GameHQStartupSupervisor Supervisor;

            public Harness(bool launchStartsProcess = false)
            {
                Supervisor = new GameHQStartupSupervisor(
                    () => Connected,
                    () => Running,
                    () => { Launches++; if (launchStartsProcess) Running = true; return true; },
                    (delay, action) => Pending.Enqueue(action),
                    _ => { },
                    message => Warnings.Add(message));
            }

            public void RunPending()
            {
                while (Pending.Count > 0) Pending.Dequeue()();
            }
        }

        [Fact]
        public void ConnectedClientNeverLaunches()
        {
            var h = new Harness { Connected = true };
            h.Supervisor.Ensure("test");
            Assert.Equal(0, h.Launches);
            Assert.Empty(h.Pending);
        }

        [Fact]
        public void RetriesAreBoundedWhenGameHQNeverStarts()
        {
            var h = new Harness();
            h.Supervisor.Ensure("test");
            h.RunPending();
            Assert.Equal(GameHQStartupSupervisor.CheckDelays.Length, h.Launches);
            Assert.False(h.Supervisor.Active);
            Assert.Contains(h.Warnings, w => w.Contains("not running"));
        }

        [Fact]
        public void RunningGameHQIsNeverLaunchedAgain()
        {
            var h = new Harness(launchStartsProcess: true);
            h.Supervisor.Ensure("test");
            h.RunPending();
            Assert.Equal(1, h.Launches);
            Assert.Contains(h.Warnings, w => w.Contains("running but"));
        }

        [Fact]
        public void StopsCheckingOnceConnected()
        {
            var h = new Harness(launchStartsProcess: true);
            h.Supervisor.Ensure("test");
            h.Connected = true;
            h.RunPending();
            Assert.Equal(1, h.Launches);
            Assert.False(h.Supervisor.Active);
            Assert.Empty(h.Warnings);
        }

        [Fact]
        public void OverlappingRequestsShareOneCycle()
        {
            var h = new Harness(launchStartsProcess: true);
            h.Supervisor.Ensure("Playnite started");
            h.Supervisor.Ensure("game starting");
            Assert.Equal(1, h.Launches);
            Assert.Single(h.Pending);
        }
    }
}
