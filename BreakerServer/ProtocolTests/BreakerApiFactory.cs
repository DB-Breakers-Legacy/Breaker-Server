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
using Microsoft.Extensions.Logging;

namespace BreakerServer.ProtocolTests;

public class BreakerApiFactory : WebApplicationFactory<Program>
{
    private SqliteConnection? _connection;

    protected override void ConfigureWebHost(IWebHostBuilder builder)
    {
        builder.ConfigureAppConfiguration((_, configuration) =>
            // No Diarkis keys configured: fresh keys per session, as in production. Tests of the fixed-key
            // development override add them through ExtraSettings.
            configuration.AddInMemoryCollection(ExtraSettings()));

        builder.ConfigureLogging(logging => logging.ClearProviders());

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

    // Settings a derived fixture adds (e.g. the official-save import folder).
    protected virtual IEnumerable<KeyValuePair<string, string?>> ExtraSettings() => [];

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        if (disposing) _connection?.Dispose();
    }
}
