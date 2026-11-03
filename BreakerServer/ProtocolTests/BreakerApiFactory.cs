using BreakerServer.Data;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Data.Sqlite;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.Extensions.Hosting;

namespace BreakerServer.ProtocolTests;

public sealed class BreakerApiFactory : WebApplicationFactory<Program>
{
    private SqliteConnection? _connection;

    protected override void ConfigureWebHost(IWebHostBuilder builder)
    {
        builder.ConfigureAppConfiguration((_, configuration) =>
            configuration.AddInMemoryCollection(new Dictionary<string, string?>
            {
                ["Breaker:DiarkisIvKey"] = string.Concat(Enumerable.Repeat("01", 16)),
                ["Breaker:DiarkisAesKey"] = string.Concat(Enumerable.Repeat("02", 16)),
                ["Breaker:DiarkisSidKey"] = string.Concat(Enumerable.Repeat("03", 16)),
                ["Breaker:DiarkisHashKey"] = string.Concat(Enumerable.Repeat("04", 16))
            }));

        builder.ConfigureTestServices(services =>
        {
            services.RemoveAll<IHostedService>();
            services.RemoveAll<DbContextOptions<BreakerDbContext>>();
            services.RemoveAll<BreakerDbContext>();

            _connection = new SqliteConnection("Data Source=:memory:");
            _connection.Open();
            services.AddDbContext<BreakerDbContext>(options => options.UseSqlite(_connection));
        });
    }

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        if (disposing) _connection?.Dispose();
    }
}
