using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

namespace Amatsukaze.Server
{
    public class MultiUserClient : IUserClient
    {
        private readonly List<IUserClient> clients = new List<IUserClient>();
        private readonly object sync = new object();

        public MultiUserClient(IUserClient client)
        {
            if (client != null)
            {
                clients.Add(client);
            }
        }

        public void Add(IUserClient client)
        {
            if (client == null)
                return;
            lock (sync)
            {
                if (clients.Contains(client) == false)
                {
                    clients.Add(client);
                }
            }
        }

        public bool TryGet<T>(out T client) where T : class
        {
            lock (sync)
            {
                client = clients.OfType<T>().FirstOrDefault();
                return client != null;
            }
        }

        private Task Broadcast(Func<IUserClient, Task> action)
        {
            IUserClient[] targets;
            lock (sync)
            {
                targets = clients.ToArray();
            }
            var tasks = targets.Select(action);
            return Task.WhenAll(tasks);
        }

        public void Finish()
        {
            IUserClient[] targets;
            lock (sync)
            {
                targets = clients.ToArray();
            }
            foreach (var client in targets)
            {
                client.Finish();
            }
        }

        public Task OnUIData(UIData data)
        {
            return Broadcast(client => client.OnUIData(data));
        }

        public Task OnConsoleUpdate(ConsoleUpdate str)
        {
            return Broadcast(client => client.OnConsoleUpdate(str));
        }

        public Task OnEncodeState(EncodeState state)
        {
            return Broadcast(client => client.OnEncodeState(state));
        }

        public Task OnLogFile(string str)
        {
            return Broadcast(client => client.OnLogFile(str));
        }

        public Task OnCommonData(CommonData data)
        {
            return Broadcast(client => client.OnCommonData(data));
        }

        public Task OnProfile(ProfileUpdate data)
        {
            return Broadcast(client => client.OnProfile(data));
        }

        public Task OnAutoSelect(AutoSelectUpdate data)
        {
            return Broadcast(client => client.OnAutoSelect(data));
        }

        public Task OnServiceSetting(ServiceSettingUpdate update)
        {
            return Broadcast(client => client.OnServiceSetting(update));
        }

        public Task OnLogoData(LogoData logoData)
        {
            return Broadcast(client => client.OnLogoData(logoData));
        }

        public Task OnDrcsData(DrcsImageUpdate update)
        {
            return Broadcast(client => client.OnDrcsData(update));
        }

        public Task OnAddResult(string requestId)
        {
            return Broadcast(client => client.OnAddResult(requestId));
        }

        public Task OnOperationResult(OperationResult result)
        {
            return Broadcast(client => client.OnOperationResult(result));
        }
    }
}
