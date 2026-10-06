using System;

namespace GameHQ.Playnite
{
    // Gets GameHQ running and connected when Playnite starts or launches a
    // game. Windows' Xbox mode defers ordinary startup apps until the first
    // switch to the desktop, so one best-effort launch is not enough: this
    // re-checks a bounded number of times and logs which case it hit.
    //
    // Never launches over a GameHQ process that is already running (a second
    // launch is forwarded to it and could bring its window over the game);
    // that case only waits for the pipe client's own reconnect loop.
    internal sealed class GameHQStartupSupervisor
    {
        internal static readonly TimeSpan[] CheckDelays =
        {
            TimeSpan.FromSeconds(15), TimeSpan.FromSeconds(45), TimeSpan.FromSeconds(90)
        };

        private readonly Func<bool> _isConnected;
        private readonly Func<bool> _isGameHQRunning;
        private readonly Func<bool> _launch;
        private readonly Action<TimeSpan, Action> _schedule;
        private readonly Action<string> _info;
        private readonly Action<string> _warn;
        private readonly object _gate = new object();

        private int _cycle;
        private int _check;
        private int _launches;
        private bool _active;

        public GameHQStartupSupervisor(Func<bool> isConnected, Func<bool> isGameHQRunning,
                                       Func<bool> launch, Action<TimeSpan, Action> schedule,
                                       Action<string> info, Action<string> warn)
        {
            _isConnected = isConnected;
            _isGameHQRunning = isGameHQRunning;
            _launch = launch;
            _schedule = schedule;
            _info = info;
            _warn = warn;
        }

        internal bool Active
        {
            get { lock (_gate) return _active; }
        }

        internal int Launches
        {
            get { lock (_gate) return _launches; }
        }

        // Starts a fresh bounded cycle unless one is already running.
        public void Ensure(string reason)
        {
            int cycle;
            lock (_gate)
            {
                if (_active || _isConnected()) return;
                _active = true;
                _check = 0;
                _launches = 0;
                cycle = ++_cycle;
            }
            _info("GameHQ not connected (" + reason + ")");
            LaunchIfNotRunning();
            ScheduleCheck(cycle);
        }

        public void Stop()
        {
            lock (_gate)
            {
                _active = false;
                ++_cycle;
            }
        }

        internal void Check(int cycle)
        {
            lock (_gate)
            {
                if (!_active || cycle != _cycle) return;
            }
            if (_isConnected())
            {
                Finish(cycle, () => _info("GameHQ connected after " + Launches + " launch attempt(s)"));
                return;
            }
            bool lastCheck;
            lock (_gate)
            {
                ++_check;
                lastCheck = _check >= CheckDelays.Length;
            }
            if (lastCheck)
            {
                Finish(cycle, () => _warn(_isGameHQRunning()
                    ? "GameHQ is running but the integration is still not connected; game context is unavailable"
                    : "GameHQ is not running after " + Launches + " launch attempt(s); captures will not work until it starts"));
                return;
            }
            LaunchIfNotRunning();
            ScheduleCheck(cycle);
        }

        private void LaunchIfNotRunning()
        {
            if (_isGameHQRunning())
            {
                _info("GameHQ is running; waiting for the integration to connect");
                return;
            }
            lock (_gate) ++_launches;
            if (!_launch())
                _warn("GameHQ could not be launched (install not found or start failed)");
        }

        private void ScheduleCheck(int cycle)
        {
            int index;
            lock (_gate) index = Math.Min(_check, CheckDelays.Length - 1);
            _schedule(CheckDelays[index], () => Check(cycle));
        }

        private void Finish(int cycle, Action log)
        {
            lock (_gate)
            {
                if (cycle != _cycle) return;
                _active = false;
            }
            log();
        }
    }
}
