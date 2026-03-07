using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server
{
    /// <summary>
    /// リソース管理
    /// </summary>
    class ResourceManager
    {
        public static readonly int MAX_GPU = 16;
        public static readonly int MAX = 100;
        private readonly object syncRoot = new object();

        private class WaitinResource
        {
            public ReqResource Req;
            public int Cost;
        }

        private TaskCompletionSource<int> waitTask = CreateWaitTask();
        private int curHDD, curCPU;
        private List<WaitinResource> waitingResources = new List<WaitinResource>();

        private int numGPU;
        private int[] curGPU;
        private int[] maxGPU;

        // エンコーダの番号
        private List<int> encodeIds = new List<int>();

        public ResourceManager()
        {
            numGPU = MAX_GPU;
            curGPU = Enumerable.Repeat(0, MAX_GPU).ToArray();
            maxGPU = Enumerable.Repeat(100, MAX_GPU).ToArray();
        }

        private static TaskCompletionSource<int> CreateWaitTask()
        {
            return new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        public void SetGPUResources(int numGPU, int[] maxGPU)
        {
            lock (syncRoot)
            {
                if(numGPU > MAX_GPU)
                {
                    throw new ArgumentException("GPU数が最大数を超えています");
                }
                if(numGPU > maxGPU.Length)
                {
                    throw new ArgumentException("numGPU > maxGPU.Count");
                }
                this.numGPU = numGPU;
                this.maxGPU = maxGPU;
                RecalculateCostsNoLock();

                // 待っている人全員に通知
                SignalAllNoLock();
            }
        }

        private void RecalculateCostsNoLock()
        {
            foreach (var w in waitingResources)
            {
                w.Cost = ResourceCostNoLock(w.Req);
            }
            waitingResources.Sort((a, b) => a.Cost - b.Cost);
        }

        private void DoRelResourceNoLock(Resource res)
        {
            curCPU -= res.Req.CPU;
            curHDD -= res.Req.HDD;
            curGPU[res.GpuIndex] -= res.Req.GPU;
            if(res.EncoderIndex != -1)
            {
                encodeIds.Remove(res.EncoderIndex);
            }
            RecalculateCostsNoLock();
        }

        // 最も余裕のあるGPUを返す
        private int MostCapableGPUNoLock()
        {
            var GPUSpace = Enumerable.Range(0, numGPU).Select(i => maxGPU[i] - curGPU[i]).ToList();
            return GPUSpace.IndexOf(GPUSpace.Max());
        }

        private int AllocateEncoderIndexNoLock()
        {
            for(int i = 0; ; i++)
            {
                if(!encodeIds.Contains(i))
                {
                    encodeIds.Add(i);
                    return i;
                }
            }
        }

        public int ResourceCost(ReqResource req)
        {
            lock (syncRoot)
            {
                return ResourceCostNoLock(req);
            }
        }

        private int ResourceCostNoLock(ReqResource req)
        {
            int gpuIndex = MostCapableGPUNoLock();
            int nextCPU = curCPU + req.CPU;
            int nextHDD = curHDD + req.HDD;
            int nextGPU = curGPU[gpuIndex] + req.GPU;
            return Math.Max(Math.Max(nextCPU - MAX, nextHDD - MAX), nextGPU - maxGPU[gpuIndex]);
        }

        // 上限を無視してリソースを確保
        public Resource ForceGetResource(ReqResource req, bool reqEncoderIndex)
        {
            lock (syncRoot)
            {
                return ForceGetResourceNoLock(req, reqEncoderIndex);
            }
        }

        private Resource ForceGetResourceNoLock(ReqResource req, bool reqEncoderIndex)
        {
            int gpuIndex = MostCapableGPUNoLock();
            curCPU += req.CPU;
            curHDD += req.HDD;
            curGPU[gpuIndex] += req.GPU;
            RecalculateCostsNoLock();

            return new Resource()
            {
                Req = req,
                GpuIndex = gpuIndex,
                EncoderIndex = reqEncoderIndex ? AllocateEncoderIndexNoLock() : -1
            };
        }

        public Resource TryGetResource(ReqResource req, bool reqEncoderIndex)
        {
            lock (syncRoot)
            {
                int cost = ResourceCostNoLock(req);

                if(cost > 0)
                {
                    // 上限を超えるのでダメ
                    return null;
                }

                if (waitingResources.Count > 0)
                {
                    // 待っている人がいる場合は、コストが最小値以下でない場合はダメ
                    if (cost > waitingResources[0].Cost)
                    {
                        return null;
                    }
                }

                // OK
                return ForceGetResourceNoLock(req, reqEncoderIndex);
            }
        }

        /// <summary>
        /// リソースを確保する
        /// </summary>
        /// <param name="req">次のフェーズで必要なリソース</param>
        /// <param name="cancelToken">キャンセルトークン</param>
        /// <returns>確保されたリソース</returns>
        public async Task<Resource> GetResource(ReqResource req, CancellationToken cancelToken, bool reqEncoderIndex)
        {
            var waiting = new WaitinResource() { Req = req };
            using var registration = cancelToken.Register((Action)(() =>
            {
                lock (syncRoot)
                {
                    if (waitingResources.Remove(waiting))
                    {
                        RecalculateCostsNoLock();
                        // キャンセルされたら一旦動かす
                        SignalAllNoLock();
                    }
                }
            }), true);

            lock (syncRoot)
            {
                waitingResources.Add(waiting);
                RecalculateCostsNoLock();
            }

            while (true)
            {
                Task wait;
                lock (syncRoot)
                {
                    // リソース確保可能 かつ 最小コスト
                    if (waitingResources.Count > 0 &&
                        waiting.Cost <= 0 &&
                        ReferenceEquals(waitingResources[0], waiting))
                    {
                        waitingResources.RemoveAt(0);
                        var res = ForceGetResourceNoLock(req, reqEncoderIndex);
                        SignalAllNoLock();
                        return res;
                    }

                    // リソースに空きがないので待つ
                    //Util.AddLog("リソース待ち: " + req.CPU + ":" + req.HDD + ":" + req.GPU);
                    wait = waitTask.Task;
                }
                await wait;
                // キャンセルされてたら例外を投げる
                cancelToken.ThrowIfCancellationRequested();
            }
        }

        private void SignalAllNoLock()
        {
            // 現在の待ちを終了させる
            waitTask.TrySetResult(0);
            // 次の待ち用に新しいタスクを生成しておく
            waitTask = CreateWaitTask();
        }

        public void ReleaseResource(Resource res)
        {
            lock (syncRoot)
            {
                //Util.AddLog("リソース解放: " + res.Req.CPU + ":" + res.Req.HDD + ":" + res.Req.GPU);
                // リソースを解放
                DoRelResourceNoLock(res);
                // 待っている人全員に通知
                SignalAllNoLock();
            }
        }
    }
}
