using Microsoft.AspNetCore.Components.Web;
using Microsoft.AspNetCore.Components.WebAssembly.Hosting;
using System.Text.Json;
using Amatsukaze.Shared;
using AmatsukazeWebUI;

var builder = WebAssemblyHostBuilder.CreateDefault(args);
builder.RootComponents.Add<App>("#app");
builder.RootComponents.Add<HeadOutlet>("head::after");

var apiBaseUrl = builder.Configuration["ApiBaseUrl"];
var baseAddress = !string.IsNullOrWhiteSpace(apiBaseUrl)
    ? new Uri(apiBaseUrl, UriKind.Absolute)
    : new Uri(builder.HostEnvironment.BaseAddress);

builder.Services.AddScoped(sp => new HttpClient { BaseAddress = baseAddress });
builder.Services.AddSingleton(new AmatsukazeWebUI.Api.ApiBaseAddress(baseAddress));
builder.Services.AddScoped<IAmatsukazeApi>(sp =>
{
    var http = sp.GetRequiredService<HttpClient>();
    var options = new JsonSerializerOptions
    {
        PropertyNameCaseInsensitive = true
    };
    return new AmatsukazeApi(http, options);
});

await builder.Build().RunAsync();
