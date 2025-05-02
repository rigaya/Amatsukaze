using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// Linux環境でWPF/Livetのない場合に使用するNotificationObject代替クラス
    /// INotifyPropertyChangedを実装したベースクラス
    /// </summary>
    public class NotificationBase : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;

        /// <summary>
        /// プロパティの変更を通知します
        /// </summary>
        /// <param name="propertyName">変更されたプロパティ名</param>
        protected virtual void RaisePropertyChanged([CallerMemberName] string propertyName = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }

        /// <summary>
        /// プロパティ値を設定し、変更があれば変更通知を発行します
        /// </summary>
        /// <typeparam name="T">プロパティの型</typeparam>
        /// <param name="storage">バッキングフィールド</param>
        /// <param name="value">新しい値</param>
        /// <param name="propertyName">プロパティ名</param>
        /// <returns>値が変更された場合はtrue、変更がなかった場合はfalse</returns>
        protected bool SetProperty<T>(ref T storage, T value, [CallerMemberName] string propertyName = null)
        {
            if (EqualityComparer<T>.Default.Equals(storage, value))
                return false;

            storage = value;
            RaisePropertyChanged(propertyName);
            return true;
        }
    }
} 