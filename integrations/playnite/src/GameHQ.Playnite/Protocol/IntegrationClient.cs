using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;
using Playnite.SDK;

namespace GameHQ.Playnite.Protocol
{
    internal enum IntegrationConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        Suspended
    }

    // Long-lived background client for the GameHQ.Local.v1 named pipe.
    internal sealed class IntegrationClient : IDisposable
    {
        public const string PipeName = "GameHQ.Local.v1";
        private const string ClientName = "GameHQ.Playnite";
        private const int ProtocolMin = 1;
        private const int ProtocolMax = 1;
        private const int ConnectTimeoutMs = 1500;
        private const int HandshakeTimeoutMs = 5000;
        private const int MinBackoffMs = 500;
        private const int MaxBackoffMs = 30000;
        private const int OutgoingQueueCapacity = 64;
        private static readonly ILogger Logger = LogManager.GetLogger(nameof(IntegrationClient));

        private sealed class Worker : IDisposable
        {
            public readonly CancellationTokenSource Stop = new CancellationTokenSource();
            public readonly SemaphoreSlim Reconnect = new SemaphoreSlim(0, 1);
            public readonly Queue<IntegrationMessage> Outgoing = new Queue<IntegrationMessage>();
            public readonly SemaphoreSlim Messages = new SemaphoreSlim(0, OutgoingQueueCapacity);
            public CancellationTokenSource Attempt;
            public bool Stopping;

            public void Dispose()
            {
                Stop.Dispose();
                Reconnect.Dispose();
                Messages.Dispose();
            }
        }

        private readonly object _gate = new object();
        private readonly string _clientVersion;
        private readonly string _pipeName;
        private Worker _worker;
        private Task _completion = Task.CompletedTask;
        private bool _startRequested;
        private bool _disposed;
        private volatile IntegrationConnectionState _state;
        private DateTime _suspendedUntilUtc;
        private volatile string _remoteAppVersion;
        private int? _protocolSelected;
        private volatile string _lastError;

        public event Action<IntegrationConnectionState> StateChanged;
        public event Action<IntegrationMessage> MessageReceived;

        public IntegrationClient(string clientVersion, string pipeName = PipeName)
        {
            _clientVersion = clientVersion;
            _pipeName = pipeName;
        }

        public IntegrationConnectionState State => _state;
        public string RemoteAppVersion => _remoteAppVersion;
        public string LastError => _lastError;
        public int? ProtocolSelected { get { lock (_gate) return _protocolSelected; } }

        // A caller can observe retirement without blocking the Playnite UI.
        internal Task Completion { get { lock (_gate) return _completion; } }

        public void Start()
        {
            lock (_gate)
            {
                if (_disposed) return;
                _startRequested = true;
                if (_worker == null) StartWorker();
            }
        }

        // Called under _gate. Only the completion of the old worker can admit a
        // pending restart, including when Stop was called from a client callback.
        private void StartWorker()
        {
            var worker = new Worker();
            _worker = worker;
            var run = Task.Run(() => RunLoop(worker));
            _completion = run.ContinueWith(completed =>
            {
                if (completed.IsFaulted)
                {
                    _lastError = completed.Exception.GetBaseException().Message;
                    Logger.Warn("GameHQ integration worker failed: " + _lastError);
                }
                lock (_gate)
                {
                    worker.Dispose();
                    _worker = null;
                    if (worker.Stopping && _startRequested) StartWorker();
                }
            }, CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default);
        }

        public void Stop()
        {
            lock (_gate) RequestStop();
        }

        private void RequestStop()
        {
            _startRequested = false;
            if (_worker == null || _worker.Stopping) return;
            _worker.Stopping = true;
            // Closing pending I/O runs off the caller's thread.
            _worker.Stop.CancelAfter(0);
        }

        public void Dispose()
        {
            lock (_gate)
            {
                _disposed = true;
                RequestStop();
            }
        }

        public void TriggerReconnect()
        {
            lock (_gate)
            {
                if (_worker == null || _worker.Stopping) return;
                _worker.Attempt?.CancelAfter(0);
                if (_worker.Reconnect.CurrentCount == 0) _worker.Reconnect.Release();
            }
        }

        public void Send(IntegrationMessage message)
        {
            lock (_gate)
            {
                if (_worker == null || _worker.Stopping || _state != IntegrationConnectionState.Connected) return;
                if (_worker.Outgoing.Count < OutgoingQueueCapacity)
                {
                    _worker.Outgoing.Enqueue(message);
                    _worker.Messages.Release();
                    return;
                }
            }
            Logger.Warn("GameHQ integration outgoing queue full, dropping " + message.Type);
        }

        private async Task RunLoop(Worker worker)
        {
            var stop = worker.Stop.Token;
            int backoffMs = MinBackoffMs;
            try
            {
                while (!stop.IsCancellationRequested)
                {
                    // A restart or a settings-page reconnect must respect the
                    // maintenance deadline announced by the previous connection.
                    var remaining = _suspendedUntilUtc - DateTime.UtcNow;
                    if (remaining > TimeSpan.Zero)
                    {
                        SetState(IntegrationConnectionState.Suspended);
                        await Task.Delay((int)Math.Min(int.MaxValue, Math.Ceiling(remaining.TotalMilliseconds)), stop);
                        continue;
                    }

                    if (_state == IntegrationConnectionState.Suspended)
                        SetState(IntegrationConnectionState.Disconnected);

                    bool connected = false;
                    using (var attempt = CancellationTokenSource.CreateLinkedTokenSource(stop))
                    {
                        lock (_gate) worker.Attempt = attempt;
                        try
                        {
                            using (var pipe = new NamedPipeClientStream(".", _pipeName, PipeDirection.InOut,
                                PipeOptions.Asynchronous, TokenImpersonationLevel.None))
                            {
                                var connect = pipe.ConnectAsync(ConnectTimeoutMs, attempt.Token);
                                // A state handler can request shutdown or throw. Always observe the pending connect.
                                try { SetState(IntegrationConnectionState.Connecting); }
                                finally { await connect; }
                                connected = await Handshake(pipe, attempt.Token);
                                if (connected)
                                {
                                    attempt.Token.ThrowIfCancellationRequested();
                                    SetState(IntegrationConnectionState.Connected);
                                    await Pump(worker, pipe, attempt.Token);
                                }
                            }
                        }
                        catch (Exception ex) when (attempt.IsCancellationRequested && IsPipeInterruption(ex)) { }
                        catch (TimeoutException) { _lastError = "GameHQ is not running or not reachable"; }
                        catch (Exception ex)
                        {
                            _lastError = ex.Message;
                            Logger.Warn("GameHQ integration connection error: " + ex.Message);
                        }
                        finally { lock (_gate) worker.Attempt = null; }
                    }

                    if (_state != IntegrationConnectionState.Suspended)
                        SetState(IntegrationConnectionState.Disconnected);
                    backoffMs = connected ? MinBackoffMs : Math.Min(MaxBackoffMs, backoffMs * 2);
                    await worker.Reconnect.WaitAsync(backoffMs, stop);
                }
            }
            catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
            finally { SetState(IntegrationConnectionState.Disconnected); }
        }

        private async Task<bool> Handshake(NamedPipeClientStream pipe, CancellationToken token)
        {
            pipe.ReadMode = PipeTransmissionMode.Byte;
            var hello = new IntegrationMessage("hello")
                .Set("client", ClientName)
                .Set("clientVersion", _clientVersion)
                .Set("protocolMin", ProtocolMin)
                .Set("protocolMax", ProtocolMax)
                .Set("requestId", Guid.NewGuid().ToString("N"));

            byte[] payload;
            using (var deadline = CancellationTokenSource.CreateLinkedTokenSource(token))
            using (deadline.Token.Register(pipe.Dispose))
            {
                deadline.CancelAfter(HandshakeTimeoutMs);
                try
                {
                    await PipeFraming.WriteFrameAsync(pipe, hello.ToUtf8Json(), deadline.Token);
                    payload = await PipeFraming.ReadFrameAsync(pipe, deadline.Token);
                    deadline.Token.ThrowIfCancellationRequested();
                }
                catch (Exception ex) when (deadline.IsCancellationRequested && !token.IsCancellationRequested && IsPipeInterruption(ex))
                {
                    _lastError = "GameHQ handshake timed out";
                    return false;
                }
            }

            if (payload == null) return false;
            var reply = IntegrationMessage.FromUtf8Json(payload);
            if (reply.Type == "hello.ack")
            {
                _remoteAppVersion = reply.GetString("appVersion");
                lock (_gate) _protocolSelected = reply.GetInt("protocolSelected");
                _lastError = null;
                return true;
            }

            if (reply.Type == "error")
            {
                _lastError = reply.GetString("code") + ": " + reply.GetString("message");
                Logger.Warn("GameHQ integration handshake rejected: " + _lastError);
            }
            return false;
        }

        private async Task Pump(Worker worker, NamedPipeClientStream pipe, CancellationToken token)
        {
            using (var session = CancellationTokenSource.CreateLinkedTokenSource(token))
            // On .NET Framework, closing the overlapped handle also interrupts
            // reads and writes already in flight when cancellation is requested.
            using (session.Token.Register(pipe.Dispose))
            {
                var writer = WriteOutgoing(worker, pipe, session.Token);
                var reader = ReadIncoming(pipe, session.Token);
                var first = await Task.WhenAny(reader, writer);
                try { await first; }
                finally
                {
                    session.Cancel();
                    var other = first == reader ? writer : reader;
                    try { await other; }
                    catch (Exception ex) when (IsPipeInterruption(ex)) { }
                }
            }
        }

        private static bool IsPipeInterruption(Exception ex) =>
            ex is OperationCanceledException || ex is ObjectDisposedException || ex is IOException;

        private async Task WriteOutgoing(Worker worker, NamedPipeClientStream pipe, CancellationToken token)
        {
            while (true)
            {
                await worker.Messages.WaitAsync(token);
                IntegrationMessage message;
                lock (_gate) message = worker.Outgoing.Dequeue();
                await PipeFraming.WriteFrameAsync(pipe, message.ToUtf8Json(), token);
            }
        }

        private async Task ReadIncoming(NamedPipeClientStream pipe, CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                var payload = await PipeFraming.ReadFrameAsync(pipe, token);
                if (payload == null) break;
                token.ThrowIfCancellationRequested();
                var message = IntegrationMessage.FromUtf8Json(payload);
                if (message.Type == "app.maintenance")
                {
                    int seconds = Math.Max(1, message.GetInt("retryAfterSeconds") ?? 30);
                    _suspendedUntilUtc = DateTime.UtcNow.AddSeconds(seconds);
                    SetState(IntegrationConnectionState.Suspended);
                }
                MessageReceived?.Invoke(message);
            }
        }

        private void SetState(IntegrationConnectionState state)
        {
            if (_state == state) return;
            _state = state;
            StateChanged?.Invoke(state);
        }
    }
}
