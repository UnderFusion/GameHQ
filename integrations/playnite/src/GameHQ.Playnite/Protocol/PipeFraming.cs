using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace GameHQ.Playnite.Protocol
{
    // GameHQ.Local.v1 frames consist of a little-endian length and a UTF-8 JSON payload.
    internal static class PipeFraming
    {
        public const int MaxFrameBytes = 64 * 1024;

        public static async Task WriteFrameAsync(Stream stream, byte[] payload, CancellationToken token)
        {
            if (payload == null || payload.Length == 0 || payload.Length > MaxFrameBytes)
                throw new InvalidOperationException("Frame payload out of bounds: " + (payload?.Length ?? -1));

            var header = BitConverter.GetBytes(payload.Length);
            if (!BitConverter.IsLittleEndian)
                Array.Reverse(header);

            await stream.WriteAsync(header, 0, header.Length, token).ConfigureAwait(false);
            await stream.WriteAsync(payload, 0, payload.Length, token).ConfigureAwait(false);
            await stream.FlushAsync(token).ConfigureAwait(false);
        }

        // Clean EOF between frames is distinct from a truncated header or payload.
        public static async Task<byte[]> ReadFrameAsync(Stream stream, CancellationToken token)
        {
            var header = await ReadExact(stream, 4, token).ConfigureAwait(false);
            if (header == null) return null;

            if (!BitConverter.IsLittleEndian)
                Array.Reverse(header);

            int length = BitConverter.ToInt32(header, 0);
            if (length <= 0 || length > MaxFrameBytes)
                throw new IOException("Invalid frame length: " + length);

            var payload = await ReadExact(stream, length, token).ConfigureAwait(false);
            if (payload == null) throw new IOException("Connection closed mid-frame");
            return payload;
        }

        private static async Task<byte[]> ReadExact(Stream stream, int count, CancellationToken token)
        {
            var buffer = new byte[count];
            int offset = 0;
            while (offset < count)
            {
                int read = await stream.ReadAsync(buffer, offset, count - offset, token).ConfigureAwait(false);
                if (read == 0)
                {
                    if (offset == 0) return null;
                    throw new IOException("Connection closed mid-frame");
                }
                offset += read;
            }
            return buffer;
        }

        public static void WriteFrame(Stream stream, byte[] payload) =>
            WriteFrameAsync(stream, payload, CancellationToken.None).GetAwaiter().GetResult();

        public static byte[] ReadFrame(Stream stream) =>
            ReadFrameAsync(stream, CancellationToken.None).GetAwaiter().GetResult();
    }
}
