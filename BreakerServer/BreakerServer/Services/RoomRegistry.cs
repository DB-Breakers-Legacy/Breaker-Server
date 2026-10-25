namespace BreakerServer.Services;

public sealed record RoomSnapshot(string Id, string OwnerId, ushort Capacity,
    uint CreatedAt, IReadOnlyList<string> Members, IReadOnlyDictionary<string, byte[]> Properties);

// Shared by matching and session transports. All mutations are serialized and
// callers receive copies so UDP callbacks cannot mutate registry state.
public sealed class RoomRegistry(TimeProvider? clock = null)
{
    private sealed class Room
    {
        public required string Id;
        public required string Owner;
        public required ushort Capacity;
        public required uint CreatedAt;
        public required DateTimeOffset Expires;
        public required bool AllowEmpty;
        public readonly List<string> Members = [];
        public readonly Dictionary<string, byte[]> Properties = new(StringComparer.Ordinal);
    }
    private readonly TimeProvider _clock = clock ?? TimeProvider.System;
    private readonly object _gate = new();
    private readonly Dictionary<string, Room> _rooms = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _membership = new(StringComparer.Ordinal);

    public RoomSnapshot Create(string owner, ushort capacity, bool allowEmpty, bool join, ushort ttl)
    {
        if (string.IsNullOrWhiteSpace(owner) || capacity == 0 || ttl == 0)
            throw new ArgumentException("Room requires an owner, capacity and TTL.");
        lock (_gate)
        {
            Cleanup();
            if (join && _membership.ContainsKey(owner)) throw new InvalidOperationException("Already in a room.");
            var now = _clock.GetUtcNow();
            var room = new Room { Id = Guid.NewGuid().ToString("N"), Owner = owner,
                Capacity = capacity, AllowEmpty = allowEmpty, CreatedAt = checked((uint)now.ToUnixTimeSeconds()),
                Expires = now.AddSeconds(ttl) };
            if (join) { room.Members.Add(owner); _membership[owner] = room.Id; }
            _rooms.Add(room.Id, room);
            return Snapshot(room);
        }
    }

    public RoomSnapshot? Join(string roomId, string userId)
    {
        lock (_gate)
        {
            Cleanup();
            if (!_rooms.TryGetValue(roomId, out var room)) return null;
            if (_membership.TryGetValue(userId, out var existing))
                return existing == roomId ? Snapshot(room) : null;
            if (room.Members.Count >= room.Capacity) return null;
            room.Members.Add(userId);
            _membership.Add(userId, roomId);
            return Snapshot(room);
        }
    }

    public RoomSnapshot? Leave(string userId)
    {
        lock (_gate)
        {
            Cleanup();
            if (!_membership.Remove(userId, out var id) || !_rooms.TryGetValue(id, out var room)) return null;
            room.Members.Remove(userId);
            if (room.Owner == userId) room.Owner = room.Members.FirstOrDefault() ?? string.Empty;
            var result = Snapshot(room);
            if (room.Members.Count == 0 && !room.AllowEmpty) _rooms.Remove(id);
            return result;
        }
    }

    public RoomSnapshot? Get(string id)
    {
        lock (_gate) { Cleanup(); return _rooms.TryGetValue(id, out var room) ? Snapshot(room) : null; }
    }

    public RoomSnapshot? ForMember(string userId)
    {
        lock (_gate)
        {
            Cleanup();
            return _membership.TryGetValue(userId, out var id) && _rooms.TryGetValue(id, out var room)
                ? Snapshot(room) : null;
        }
    }

    public bool SetOwner(string actor, string owner)
    {
        lock (_gate)
        {
            Cleanup();
            if (!_membership.TryGetValue(actor, out var id) || !_rooms.TryGetValue(id, out var room) ||
                room.Owner != actor || !room.Members.Contains(owner)) return false;
            room.Owner = owner;
            return true;
        }
    }

    public bool SetProperties(string member, IReadOnlyDictionary<string, byte[]> properties)
    {
        lock (_gate)
        {
            Cleanup();
            if (!_membership.TryGetValue(member, out var id) || !_rooms.TryGetValue(id, out var room)) return false;
            foreach (var (key, value) in properties) room.Properties[key] = value.ToArray();
            return true;
        }
    }

    private void Cleanup()
    {
        var now = _clock.GetUtcNow();
        foreach (var room in _rooms.Values.Where(x => x.Expires <= now).ToArray())
        {
            foreach (var member in room.Members) _membership.Remove(member);
            _rooms.Remove(room.Id);
        }
    }

    private static RoomSnapshot Snapshot(Room room) => new(room.Id, room.Owner, room.Capacity,
        room.CreatedAt, room.Members.ToArray(), room.Properties.ToDictionary(x => x.Key, x => x.Value.ToArray()));
}
