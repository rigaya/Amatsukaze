using System;

namespace Amatsukaze.Shared
{
    public sealed class ApiResult<T>
    {
        public bool Ok { get; init; }
        public int StatusCode { get; init; }
        public string? Error { get; init; }
        public T? Data { get; init; }

        public static ApiResult<T> Success(T data, int statusCode = 200)
        {
            return new ApiResult<T> { Ok = true, Data = data, StatusCode = statusCode };
        }

        public static ApiResult<T> Fail(int statusCode, string error)
        {
            return new ApiResult<T> { Ok = false, StatusCode = statusCode, Error = error };
        }
    }
}
