using System;

namespace Amatsukaze.Shared
{
    public sealed class ApiResult<T>
    {
        public bool Ok { get; init; }
        public int StatusCode { get; init; }
        public string? Error { get; init; }
        /// <summary>RESTのBadRequest等で返す機械可読なエラー種別（例: カット調整の一時フォルダ欠如）。</summary>
        public string? ErrorCode { get; init; }
        public T? Data { get; init; }

        public static ApiResult<T> Success(T data, int statusCode = 200)
        {
            return new ApiResult<T> { Ok = true, Data = data, StatusCode = statusCode };
        }

        public static ApiResult<T> Fail(int statusCode, string error, string? errorCode = null)
        {
            return new ApiResult<T> { Ok = false, StatusCode = statusCode, Error = error, ErrorCode = errorCode };
        }
    }
}
