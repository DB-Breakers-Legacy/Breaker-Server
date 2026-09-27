using Microsoft.EntityFrameworkCore;

namespace BreakerServer.Data;

public sealed class BreakerDbContext(DbContextOptions<BreakerDbContext> options) : DbContext(options)
{
    public DbSet<PatrollerUser> PatrollerUsers => Set<PatrollerUser>();
    public DbSet<MessageBot> MessageBots => Set<MessageBot>();

    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<PatrollerUser>().HasKey(x => x.UserId);
        modelBuilder.Entity<MessageBot>().HasKey(x => x.MessageId);
        modelBuilder.Entity<MessageBot>().Property(x => x.MessageId).HasMaxLength(32);
    }
}
