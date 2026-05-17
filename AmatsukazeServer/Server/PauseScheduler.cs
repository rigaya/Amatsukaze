using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Threading.Tasks.Dataflow;

namespace Amatsukaze.Server
{
    class PauseScheduler
    {
        private EncodeServer server;
        private WorkerPool workerPool;

        private Task timerThread;
        private BufferBlock<int> timerQ = new BufferBlock<int>();

        public PauseScheduler(EncodeServer server, WorkerPool workerPool)
        {
            this.server = server;
            this.workerPool = workerPool;
            NotifySettingChanged();
        }

        private async Task TimerFunc()
        {
            Task<bool> timerQRecvTask = timerQ.OutputAvailableAsync();

            try
            {
                while (true)
                {
                    var setting = server.AppData_.setting;
                    var now = DateTime.Now;
                    var isPauseHour = setting.EnableRunHours && !setting.RunHours[now.Hour];

                    try
                    {
                        workerPool.SetPause(isPauseHour, true);
                    }
                    catch (Exception e)
                    {
                        Util.AddLog("稼働時間設定のキュー停止状態更新に失敗", e);
                    }

                    var suspend = isPauseHour && setting.RunHoursSuspendEncoders;
                    // workersの数が変わってるかもしれないので毎回行う
                    foreach(var worker in workerPool.Workers.OfType<TranscodeWorker>().ToArray())
                    {
                        try
                        {
                            worker.SetSuspend(suspend, true);
                        }
                        catch (Exception e)
                        {
                            Util.AddLog(worker.GetItemId(), "稼働時間設定のエンコーダ停止状態更新に失敗", e);
                        }
                    }

                    if (setting.EnableRunHours == false)
                    {
                        break;
                    }

                    await server.RequestState();

                    var future = now.AddMinutes(60);
                    var elapsed = new DateTime(future.Year, future.Month, future.Day, future.Hour, 0, 0) - now;

                    if(await Task.WhenAny(timerQRecvTask, Task.Delay(elapsed)) == timerQRecvTask)
                    {
                        if(timerQRecvTask.Result == false)
                        {
                            // 完了した
                            break;
                        }
                        timerQ.Receive();
                        timerQRecvTask = timerQ.OutputAvailableAsync();
                    }
                }
            }
            catch (Exception e)
            {
                Util.AddLog("稼働時間設定タイマーでエラーが発生", e);
            }
            finally
            {
                timerThread = null;
                try
                {
                    await server.RequestState();
                }
                catch (Exception e)
                {
                    Util.AddLog("稼働時間設定タイマー終了時の状態通知に失敗", e);
                }
            }
        }

        public void NotifySettingChanged()
        {
            if(timerThread == null && server.AppData_.setting.EnableRunHours)
            {
                timerThread = TimerFunc();
            }
            else if(timerThread != null)
            {
                timerQ.Post(0);
            }
        }

        public void Complete()
        {
            timerQ.Complete();
        }
    }
}
