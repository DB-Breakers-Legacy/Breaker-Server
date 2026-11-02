using BreakerServer.Authentication;
using BreakerServer.Configuration;
using BreakerServer.Data;
using BreakerServer.Endpoints;
using BreakerServer.Infrastructure;
using BreakerServer.Network;
using BreakerServer.Services;
using Microsoft.EntityFrameworkCore;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.ConfigureKestrel(options =>
    options.Limits.MaxRequestBodySize = MessagePackHttp.MaxBodyBytes);

builder.Services.AddOptions<BreakerOptions>()
    .Bind(builder.Configuration.GetSection(BreakerOptions.SectionName))
    .Validate(HasValidDiarkisKeys, "Breaker Diarkis keys must be configured as hexadecimal strings with valid lengths.")
    .ValidateOnStart();
builder.Services.AddDbContext<BreakerDbContext>(options =>
    options.UseSqlite(builder.Configuration.GetConnectionString("Breaker") ?? "Data Source=breaker.db"));
builder.Services.AddSingleton<ResponseCatalog>();
builder.Services.AddSingleton<StarterDataStore>();
builder.Services.AddSingleton<SessionTokenService>();
builder.Services.AddSingleton<SessionKeyRegistry>();
builder.Services.AddSingleton<AuthSessionRegistry>();
builder.Services.AddHostedService<DiarkisUdpService>();
builder.Services.AddHostedService<DiarkisSessionUdpService>();
builder.Services.AddHostedService<StunUdpService>();
builder.Services.AddScoped<ApiResponseService>();
builder.Services.AddScoped<AccountService>();
builder.Services.AddScoped<UserAuthService>();
builder.Services.AddScoped<RequestIdentityResolver>();

var app = builder.Build();
app.UseMiddleware<BadRequestMiddleware>();
app.UseMiddleware<RequestTraceMiddleware>();
app.UseMiddleware<CdnHeadersMiddleware>();

using (var scope = app.Services.CreateScope())
    scope.ServiceProvider.GetRequiredService<BreakerDbContext>().Database.EnsureCreated();

app.MapGet("/health", () => Results.Ok(new
{
    service = "Breaker-Server",
    status = "ok",
    mode = "local-emulation"
}));

app.MapGet("/emulation/status", (Microsoft.Extensions.Options.IOptions<BreakerOptions> options) => Results.Ok(new
{
    backend = options.Value.BackendBaseUrl,
    udp = new { host = options.Value.UdpHost, port = options.Value.UdpPort, sessionPort = options.Value.UdpSessionPort },
    stun = new { host = options.Value.StunHost, ports = options.Value.StunPorts }
}));

app.MapBreakerApi();
app.Run();

static bool HasValidDiarkisKeys(BreakerOptions options)
{
    try
    {
        BreakerServer.Protocol.Diarkis.SecureEnvelope.FromHex(
            options.DiarkisSidKey,
            options.DiarkisAesKey,
            options.DiarkisIvKey,
            options.DiarkisHashKey);
        return true;
    }
    catch (Exception ex) when (ex is FormatException or InvalidOperationException)
    {
        return false;
    }
}

public partial class Program;
