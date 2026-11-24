using System;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using GameHQ.Playnite.Protocol;
using Xunit;

namespace GameHQ.Playnite.Tests
{
    public class IntegrationClientLifecycleTests
    {
        private static readonly TimeSpan Deadline = TimeSpan.FromSeconds(4);
        private const string Ack = "{\"type\":\"hello.ack\",\"appVersion\":\"test-server\",\"protocolSelected\":1}";

        private sealed class Peer : IDisposable
        {
            public readonly string Name;
            public readonly NamedPipeServerStream Pipe;

            public Peer(string name = null)
            {
                Name = name ?? "GameHQ.Test." + Guid.NewGuid().ToString("N");
                Pipe = new NamedPipeServerStream(Name, PipeDirection.InOut, 2,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 4096, 4096);
            }

            public async Task AcceptHello()
            {
                await Pipe.WaitForConnectionAsync().WaitAsync(Deadline);
                var hello = IntegrationMessage.FromUtf8Json(await ReadFrame());
                Assert.Equal("hello", hello.Type);
                Assert.Equal("GameHQ.Playnite", hello.GetString("client"));
                Assert.Equal(1, hello.GetInt("protocolMin"));
                Assert.Equal(1, hello.GetInt("protocolMax"));
            }

            public Task<byte[]> ReadFrame() =>
                Task.Run(() => PipeFraming.ReadFrame(Pipe)).WaitAsync(Deadline);

            public void Reply(string json) => PipeFraming.WriteFrame(Pipe, Encoding.UTF8.GetBytes(json));

            // On Windows this barrier proves the client consumed the first byte
            // and still needs three bytes to finish reading the frame header.
            public async Task StartPartialFrame()
            {
                Pipe.WriteByte(32);
                if (Environment.OSVersion.Platform == PlatformID.Win32NT)
                    await Task.Run(Pipe.WaitForPipeDrain).WaitAsync(Deadline);
            }

            public void Dispose() => Pipe.Dispose();
        }

        private static TaskCompletionSource<bool> Signal() =>
            new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);

        private static Task WhenState(IntegrationClient client, IntegrationConnectionState expected)
        {
            var seen = Signal();
            client.StateChanged += state => { if (state == expected) seen.TrySetResult(true); };
            return seen.Task;
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task ShutdownInterruptsUnansweredHandshake(bool dispose)
        {
            using var peer = new Peer();
            using var client = new IntegrationClient("test", peer.Name);
            client.Start();
            await peer.AcceptHello();
            await peer.StartPartialFrame();
            if (dispose) client.Dispose();
            else client.Stop();
            Assert.Null(await peer.ReadFrame());
            await client.Completion.WaitAsync(Deadline);
            Assert.Equal(IntegrationConnectionState.Disconnected, client.State);
            Assert.Null(client.LastError);
        }

        [Fact]
        public async Task ConcurrentStopAndDisposeInterruptBlockedRead()
        {
            using var peer = new Peer();
            using var client = new IntegrationClient("test", peer.Name);
            var connected = WhenState(client, IntegrationConnectionState.Connected);
            client.Start();
            await peer.AcceptHello();
            peer.Reply(Ack);
            await connected.WaitAsync(Deadline);
            await peer.StartPartialFrame();
            using var ready = new Barrier(2);
            var stop = Task.Run(() => { Assert.True(ready.SignalAndWait(Deadline)); client.Stop(); });
            var dispose = Task.Run(() => { Assert.True(ready.SignalAndWait(Deadline)); client.Dispose(); });
            await Task.WhenAll(stop, dispose).WaitAsync(Deadline);
            Assert.Null(await peer.ReadFrame());
            await client.Completion.WaitAsync(Deadline);
            Assert.Equal(IntegrationConnectionState.Disconnected, client.State);
            Assert.Null(client.LastError);
        }

        [Fact]
        public async Task RestartWaitsForThePreviousCallbackToReturn()
        {
            using var first = new Peer();
            using var client = new IntegrationClient("test", first.Name);
            var release = Signal();
            var entered = Signal();
            int inCallback = 0;
            int overlaps = 0;
            int connects = 0;
            client.MessageReceived += message =>
            {
                Interlocked.Exchange(ref inCallback, 1);
                entered.TrySetResult(true);
                try { release.Task.Wait(TimeSpan.FromSeconds(12)); }
                finally { Interlocked.Exchange(ref inCallback, 0); }
            };
            client.StateChanged += state =>
            {
                if (state != IntegrationConnectionState.Connecting) return;
                Interlocked.Increment(ref connects);
                if (Volatile.Read(ref inCallback) != 0) Interlocked.Increment(ref overlaps);
            };

            client.Start();
            await first.AcceptHello();
            first.Reply(Ack);
            first.Reply("{\"type\":\"hold\"}");
            await entered.Task.WaitAsync(Deadline);
            using var next = new Peer(first.Name);
            var previousWorker = client.Completion;
            try
            {
                for (int i = 0; i < 8; ++i) { client.Stop(); client.Start(); }
                Assert.Null(await first.ReadFrame());
                Assert.Same(previousWorker, client.Completion);
                Assert.False(previousWorker.IsCompleted);
                Assert.Equal(0, Volatile.Read(ref overlaps));
            }
            finally { release.TrySetResult(true); }
            var connected = WhenState(client, IntegrationConnectionState.Connected);
            await next.AcceptHello();
            next.Reply(Ack);
            await connected.WaitAsync(Deadline);
            await previousWorker.WaitAsync(Deadline);
            Assert.Equal(2, Volatile.Read(ref connects));
            Assert.Equal(0, Volatile.Read(ref overlaps));
            Assert.Equal("test-server", client.RemoteAppVersion);
            Assert.Equal(1, client.ProtocolSelected);
            Assert.Null(client.LastError);
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task MaintenanceSuppressesReconnectUntilTheWindowExpires(bool restart)
        {
            using var first = new Peer();
            using var client = new IntegrationClient("test", first.Name);
            var suspended = WhenState(client, IntegrationConnectionState.Suspended);
            client.Start();
            await first.AcceptHello();
            first.Reply(Ack);
            var elapsed = Stopwatch.StartNew();
            first.Reply("{\"type\":\"app.maintenance\",\"retryAfterSeconds\":1}");
            await suspended.WaitAsync(Deadline);
            using var next = new Peer(first.Name);
            if (restart)
            {
                client.Stop();
                client.Start();
            }
            else client.TriggerReconnect();
            Assert.Null(await first.ReadFrame());
            await next.AcceptHello();
            Assert.True(elapsed.Elapsed >= TimeSpan.FromSeconds(1));
            var connected = WhenState(client, IntegrationConnectionState.Connected);
            next.Reply(Ack);
            await connected.WaitAsync(Deadline);
        }

        [Fact]
        public async Task SilentPeerTimesOutAndAllowsAFreshHandshake()
        {
            using var first = new Peer();
            using var client = new IntegrationClient("test", first.Name);
            var disconnected = WhenState(client, IntegrationConnectionState.Disconnected);
            client.Start();
            await first.AcceptHello();
            await first.StartPartialFrame();
            await disconnected.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.Equal("GameHQ handshake timed out", client.LastError);
            Assert.Null(await first.ReadFrame());
            using var next = new Peer(first.Name);
            var connected = WhenState(client, IntegrationConnectionState.Connected);
            client.TriggerReconnect();
            await next.AcceptHello();
            next.Reply(Ack);
            await connected.WaitAsync(Deadline);
            Assert.Null(client.LastError);
        }

        [Fact]
        public async Task OutgoingQueuePreservesOrderAndDropsTheSixtyFifthMessage()
        {
            using var peer = new Peer();
            using var client = new IntegrationClient("test", peer.Name);
            client.StateChanged += state =>
            {
                if (state == IntegrationConnectionState.Connected)
                    for (int i = 0; i < 65; ++i)
                        client.Send(new IntegrationMessage("queued").Set("index", i));
            };
            client.Start();
            await peer.AcceptHello();
            peer.Reply(Ack);
            for (int i = 0; i < 64; ++i)
                Assert.Equal(i, IntegrationMessage.FromUtf8Json(await peer.ReadFrame()).GetInt("index"));
            client.Send(new IntegrationMessage("end"));
            Assert.Equal("end", IntegrationMessage.FromUtf8Json(await peer.ReadFrame()).Type);
        }

        [Fact]
        public async Task DisposeDuringPendingConnectRetiresWorker()
        {
            var name = "GameHQ.Test." + Guid.NewGuid().ToString("N");
            using var busy = new NamedPipeServerStream(name, PipeDirection.InOut, 1,
                PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
            using var occupant = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.Asynchronous);
            await Task.WhenAll(busy.WaitForConnectionAsync(), occupant.ConnectAsync()).WaitAsync(Deadline);
            using var client = new IntegrationClient("test", name);
            var connecting = WhenState(client, IntegrationConnectionState.Connecting);
            client.Start();
            await connecting.WaitAsync(Deadline);
            client.Dispose();
            await client.Completion.WaitAsync(Deadline);
            Assert.Equal(IntegrationConnectionState.Disconnected, client.State);
            Assert.Null(client.LastError);
        }

        [Fact]
        public async Task StopInterruptsSendToAPeerThatStopsReading()
        {
            using var peer = new Peer();
            using var client = new IntegrationClient("test", peer.Name);
            var connected = WhenState(client, IntegrationConnectionState.Connected);
            client.Start();
            await peer.AcceptHello();
            peer.Reply(Ack);
            await connected.WaitAsync(Deadline);
            client.Send(new IntegrationMessage("large").Set("body", new string('x', 60 * 1024)));
            var firstByte = new byte[1];
            Assert.Equal(1, await peer.Pipe.ReadAsync(firstByte, 0, 1).WaitAsync(Deadline));
            client.Stop();
            await client.Completion.WaitAsync(Deadline);
            Assert.Equal(IntegrationConnectionState.Disconnected, client.State);
            Assert.Null(client.LastError);
        }

        [Fact]
        public void LateLifecycleCallsAreSafeAfterDispose()
        {
            var client = new IntegrationClient("test", "GameHQ.Test." + Guid.NewGuid().ToString("N"));
            client.Dispose();
            var retired = client.Completion;
            client.TriggerReconnect();
            client.Stop();
            client.Start();
            client.Send(new IntegrationMessage("ignored"));
            client.Dispose();
            Assert.Same(retired, client.Completion);
            Assert.Equal(IntegrationConnectionState.Disconnected, client.State);
        }
    }
}
