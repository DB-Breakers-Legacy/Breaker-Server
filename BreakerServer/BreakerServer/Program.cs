using BreakerServer.Authentication;
using BreakerServer.Configuration;
using BreakerServer.Data;
using BreakerServer.Endpoints;
using BreakerServer.Infrastructure;
using BreakerServer.Network;
using BreakerServer.Services;
using Microsoft.EntityFrameworkCore;

var builder = WebApplication.CreateBuilder(args);

builder.Services.Configure<BreakerOptions>(builder.Configuration.GetSection(BreakerOptions.SectionName));
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
