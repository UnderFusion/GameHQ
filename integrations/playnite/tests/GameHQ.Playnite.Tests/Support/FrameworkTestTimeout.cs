#if NETFRAMEWORK
using System;
using System.Threading;
using System.Threading.Tasks;

namespace GameHQ.Playnite.Tests
{
    internal static class FrameworkTestTimeout
    {
        public static async Task WaitAsync(this Task task, TimeSpan timeout)
        {
            using (var timer = new CancellationTokenSource())
            {
                if (await Task.WhenAny(task, Task.Delay(timeout, timer.Token)) != task)
                    throw new TimeoutException("Test operation exceeded " + timeout);
                timer.Cancel();
                await task;
            }
        }

        public static async Task<T> WaitAsync<T>(this Task<T> task, TimeSpan timeout)
        {
            await ((Task)task).WaitAsync(timeout);
            return await task;
        }
    }
}
#endif
