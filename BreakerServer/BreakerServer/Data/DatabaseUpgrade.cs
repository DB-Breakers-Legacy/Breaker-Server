using Microsoft.EntityFrameworkCore;

namespace BreakerServer.Data;

// EnsureCreated only builds a database that does not exist yet; it never adds columns to an existing one.
// Columns added to PatrollerUser after a server's database was first made are added here, empty, so every
// account keeps what it already has.
public static class DatabaseUpgrade
{
    private static readonly string[] PlayerTextColumns = ["StatusJson", "ItemsJson", "ImportedFrom"];

    public static void Run(BreakerDbContext db)
    {
        var connection = db.Database.GetDbConnection();
        var opened = connection.State != System.Data.ConnectionState.Open;
        if (opened) connection.Open();
        try
        {
            var existing = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            using (var info = connection.CreateCommand())
            {
                info.CommandText = "PRAGMA table_info(\"PatrollerUsers\")";
                using var reader = info.ExecuteReader();
                while (reader.Read()) existing.Add(reader.GetString(1));
            }

            foreach (var column in PlayerTextColumns.Where(c => !existing.Contains(c)))
            {
                using var add = connection.CreateCommand();
                add.CommandText = $"ALTER TABLE \"PatrollerUsers\" ADD COLUMN \"{column}\" TEXT NOT NULL DEFAULT ''";
                add.ExecuteNonQuery();
            }
        }
        finally
        {
            if (opened) connection.Close();
        }
    }
}
