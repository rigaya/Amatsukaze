using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Net;
using System.Net.Sockets;

namespace Amatsukaze.Server.Rest
{
    // 起動時に確定させ、接続元とCORSで同じ許可範囲を使う。
    internal sealed class RestAccessPolicy
    {
        private readonly IPNetwork[] additionalNetworks;

        private RestAccessPolicy(IPNetwork[] networks)
        {
            additionalNetworks = networks;
        }

        internal static RestAccessPolicy Parse(string value, Action<string> log)
        {
            var networks = new List<IPNetwork>();
            foreach (var entry in (value ?? string.Empty).Split(','))
            {
                var item = entry.Trim();
                if (item.Length == 0)
                    continue;
                if (!TryParseNetwork(item, out var network))
                {
                    log?.Invoke("[REST] 警告: AMT_REST_ADD_ALLOWに不正な項目があります。追加設定全体を無効にし、既存の許可範囲で動作します。");
                    return new RestAccessPolicy(Array.Empty<IPNetwork>());
                }
                networks.Add(network);
            }
            var result = networks.Distinct().ToArray();
            if (result.Length > 0)
                log?.Invoke("[REST] AMT_REST_ADD_ALLOWの追加許可範囲: " + string.Join(", ", result.Select(n => n.ToString())));
            return new RestAccessPolicy(result);
        }

        internal bool IsAllowed(IPAddress address)
        {
            if (address.IsIPv4MappedToIPv6)
                address = address.MapToIPv4();
            return IsAllowedByDefault(address) || additionalNetworks.Any(n => n.Contains(address));
        }

        private static bool TryParseNetwork(string item, out IPNetwork network)
        {
            network = default;
            var parts = item.Split('/');
            if (parts.Length > 2 || !IPAddress.TryParse(parts[0], out var address))
                return false;
            // IPv4の省略形・整数表記や角括弧付きIPv6は設定ミスとして扱う。
            if (parts[0].Contains('[') || parts[0].Contains(']') || parts[0].Contains('%') ||
                (address.AddressFamily == AddressFamily.InterNetwork && address.ToString() != parts[0]))
                return false;
            var prefix = address.AddressFamily == AddressFamily.InterNetwork ? 32 : 128;
            if (parts.Length == 2 && (!int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out prefix) ||
                prefix < 0 || prefix > (address.AddressFamily == AddressFamily.InterNetwork ? 32 : 128)))
                return false;
            if (address.IsIPv4MappedToIPv6)
            {
                if (prefix < 96)
                    return false;
                address = address.MapToIPv4();
                prefix -= 96;
            }
            var bytes = address.GetAddressBytes();
            for (var i = 0; i < bytes.Length; i++)
            {
                var bits = Math.Clamp(prefix - i * 8, 0, 8);
                bytes[i] &= (byte)(0xFF << (8 - bits));
            }
            network = new IPNetwork(new IPAddress(bytes), prefix);
            return true;
        }

        private static bool IsAllowedByDefault(IPAddress address)
        {
            if (IPAddress.IsLoopback(address))
            {
                return true;
            }

            if (address.IsIPv4MappedToIPv6)
            {
                address = address.MapToIPv4();
            }

            if (address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
            {
                var bytes = address.GetAddressBytes();
                if (bytes[0] == 10)
                {
                    return true;
                }
                if (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31)
                {
                    return true;
                }
                if (bytes[0] == 192 && bytes[1] == 168)
                {
                    return true;
                }
                return false;
            }

            if (address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6)
            {
                if (address.IsIPv6LinkLocal || address.IsIPv6SiteLocal)
                {
                    return true;
                }
                var bytes = address.GetAddressBytes();
                return (bytes[0] & 0xFE) == 0xFC; // Unique local (fc00::/7)
            }

            return false;
        }

    }
}
