using System;
using System.Collections.Concurrent;
using System.IO;
using Amatsukaze.Lib;

namespace Amatsukaze.Server.Rest
{
    public sealed class LogoPreviewSession : IDisposable
    {
        public string Id { get; }
        public int QueueItemId { get; }
        public int ServiceId { get; }
        public string FilePath { get; }
        public DateTime LastAccessUtc { get; private set; }
        public object SyncRoot { get; } = new object();

        private readonly AMTContext ctx;
        private readonly MediaFile mediaFile;

        public LogoPreviewSession(string id, int queueItemId, int serviceId, string filePath)
        {
            Id = id;
            QueueItemId = queueItemId;
            ServiceId = serviceId;
            FilePath = filePath;
            ctx = new AMTContext();
            mediaFile = new MediaFile(ctx, filePath, serviceId);
            Touch();
        }

        public void Touch()
        {
            LastAccessUtc = DateTime.UtcNow;
        }

        public object GetFrame(float pos)
        {
            Touch();
            lock (SyncRoot)
            {
                return mediaFile.GetFrame(pos);
            }
        }

        public void Dispose()
        {
            mediaFile?.Dispose();
            ctx?.Dispose();
        }
    }

    public class LogoPreviewService
    {
        private readonly RestStateStore state;
        private readonly ConcurrentDictionary<string, LogoPreviewSession> sessions = new ConcurrentDictionary<string, LogoPreviewSession>();
        private readonly TimeSpan sessionTtl = TimeSpan.FromSeconds(60);

        public LogoPreviewService(RestStateStore state)
        {
            this.state = state;
        }

        public bool TryCreateSession(LogoPreviewSessionRequest request, out LogoPreviewSessionResponse response, out string error)
        {
            response = null;
            error = null;

            if (request == null || request.QueueItemId <= 0)
            {
                error = "QueueItemIdが不正です";
                return false;
            }

            if (!state.TryGetQueueItem(request.QueueItemId, out var item) || string.IsNullOrEmpty(item.SrcPath))
            {
                error = "キューの入力ファイルが見つかりません";
                return false;
            }

            if (!ServerSupport.TryResolveInputFilePath(item.SrcPath, out var filePath))
            {
                error = "入力ファイルが存在しません";
                return false;
            }

            var serviceId = item.ServiceId > 0 ? item.ServiceId : request.ServiceId;
            if (serviceId <= 0)
            {
                error = "サービスIDが取得できません";
                return false;
            }

            CleanupExpired();

            var sessionId = Guid.NewGuid().ToString("N");
            try
            {
                var session = new LogoPreviewSession(sessionId, request.QueueItemId, serviceId, filePath);
                sessions[sessionId] = session;
            }
            catch (IOException ex)
            {
                error = ex.Message;
                return false;
            }

            response = new LogoPreviewSessionResponse()
            {
                SessionId = sessionId
            };
            return true;
        }

        public LogoPreviewSession GetSession(string sessionId)
        {
            CleanupExpired();
            if (string.IsNullOrEmpty(sessionId))
            {
                return null;
            }
            sessions.TryGetValue(sessionId, out var session);
            return session;
        }

        public bool RemoveSession(string sessionId)
        {
            if (string.IsNullOrEmpty(sessionId))
            {
                return false;
            }
            if (sessions.TryRemove(sessionId, out var session))
            {
                session.Dispose();
                return true;
            }
            return false;
        }

        private void CleanupExpired()
        {
            var now = DateTime.UtcNow;
            foreach (var pair in sessions)
            {
                if (now - pair.Value.LastAccessUtc > sessionTtl)
                {
                    if (sessions.TryRemove(pair.Key, out var session))
                    {
                        session.Dispose();
                    }
                }
            }
        }
    }
}
