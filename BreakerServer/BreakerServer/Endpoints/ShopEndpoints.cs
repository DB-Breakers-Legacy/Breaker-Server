using BreakerServer.Authentication;
using BreakerServer.Infrastructure;
using BreakerServer.Services;

namespace BreakerServer.Endpoints;

// Test shop (2026-11-13). The official shop replies were never captured, so the formats were read from the game's
// own reply readers (see "breakers server\official api reference\SHOP_FORMAT_NOTES.md"):
//   shop/get_category_list        -> [status, [[id, name, productCount, updatedTime, text], ...], uint]
//   shop/get_info_category        -> [status, categoryId, text, text, productsA[], lineup[]]
//                                    lineup entry: 14 fields, the last one the list of items you get
//                                    [[productId, itemType ("RivalID", "CostumeItemID", ...), itemId, count]]
//   shop/get_info_special_category -> [status, list, list]   (guess: empty lists)
//   shop/payment                   -> [status, uint, uint]   (request args [product type, product id, uint, text])
// The shop content is Assets/shop.json, read again on every request, so it can be changed while the server runs.
// Price = last number of a lineup item entry, currency = lineup [4] (2 = Zeni); see the note in shop.json.
public static class ShopEndpoints
{
    public static IEndpointRouteBuilder MapShopEndpoints(this IEndpointRouteBuilder app)
    {
        var api = app.MapGroup("/dbtb-prd/{title}/api");

        api.MapPost("/shop/get_category_list", async (
            HttpRequest request,
            ApiResponseService responses,
            IWebHostEnvironment env,
            ILoggerFactory logs,
            CancellationToken ct) =>
        {
            var log = logs.CreateLogger("BreakerServer.Shop");
            var args = await ReadArgsAsync(request, ct);
            var categories = new List<object?>();
            foreach (var category in Categories(env))
            {
                categories.Add(new object?[]
                {
                    Get(category, "id"),
                    Get(category, "name") ?? string.Empty,
                    ObjectTree.List(Get(category, "lineup") ?? new List<object?>()).Count,
                    Get(category, "updated") ?? "2026-11-13 00:00:00",
                    Get(category, "extra") ?? string.Empty
                });
            }

            var reply = new object?[] { 0, categories, 0 };
            log.LogInformation("Shop get_category_list args={Args} reply={Reply}", ObjectTree.ToJson(args),
                ObjectTree.ToJson(reply));
            return responses.Respond(reply);
        });

        api.MapPost("/shop/get_info_category", async (
            HttpRequest request,
            ApiResponseService responses,
            IWebHostEnvironment env,
            ILoggerFactory logs,
            CancellationToken ct) =>
        {
            var log = logs.CreateLogger("BreakerServer.Shop");
            var args = await ReadArgsAsync(request, ct);
            var categories = Categories(env);

            // The request carries a language code, the category id and an offset (paged by 500); the order is not
            // confirmed yet, so take the first number that is one of our category ids.
            var numbers = args.Where(IsNumber).Select(ObjectTree.Int32).ToList();
            var category = categories.FirstOrDefault(c => numbers.Contains(ObjectTree.Int32(Get(c, "id"))))
                           ?? categories.FirstOrDefault();

            object?[] reply;
            if (category is null)
            {
                reply = new object?[] { 0, 0, string.Empty, string.Empty, new List<object?>(), new List<object?>() };
            }
            else
            {
                reply = new object?[]
                {
                    0,
                    Get(category, "id"),
                    Get(category, "name") ?? string.Empty,
                    Get(category, "text2") ?? string.Empty,
                    new List<object?>(),
                    Get(category, "lineup") ?? new List<object?>()
                };
            }

            log.LogInformation("Shop get_info_category args={Args} reply={Reply}", ObjectTree.ToJson(args),
                ObjectTree.ToJson(reply));
            return responses.Respond(reply);
        });

        api.MapPost("/shop/get_info_special_category", async (
            HttpRequest request,
            ApiResponseService responses,
            ILoggerFactory logs,
            CancellationToken ct) =>
        {
            var log = logs.CreateLogger("BreakerServer.Shop");
            var args = await ReadArgsAsync(request, ct);
            var reply = new object?[] { 0, new List<object?>(), new List<object?>() };
            log.LogInformation("Shop get_info_special_category args={Args} reply={Reply}", ObjectTree.ToJson(args),
                ObjectTree.ToJson(reply));
            return responses.Respond(reply);
        });

        // Buying. The game (MenuShop sub_140A082C0 -> sub_1406E4E80) sends [product type, product id, Possible_
        // category value, ""]; no price or currency, so the server looks them up in shop.json. The game takes the
        // price off its own counter after a successful reply (sub_140A189E0), so the saved values must match.
        api.MapPost("/shop/payment", async (
            HttpRequest request,
            ApiResponseService responses,
            IWebHostEnvironment env,
            RequestIdentityResolver identity,
            AccountService accounts,
            ILoggerFactory logs,
            CancellationToken ct) =>
        {
            var log = logs.CreateLogger("BreakerServer.Shop");
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var args = ArgsOf(body);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);

            var numbers = args.Where(IsNumber).Select(ObjectTree.Int64).ToList();
            var productId = numbers.Count > 1 ? numbers[1] : -1;
            var product = FindProduct(Categories(env), productId);
            if (product is null)
            {
                log.LogWarning("Shop payment refused: product {Product} not in shop.json, args={Args}", productId,
                    ObjectTree.ToJson(args));
                return responses.Respond(new object?[] { RefusedStatus, 0, 0 });
            }

            var items = ObjectTree.List(product[13]).Select(i => ObjectTree.List(i, 4)).ToList();
            var currency = ObjectTree.Int32(product[4]);
            var price = items.Sum(i => ObjectTree.Int64(i[3]));

            // Currency -> place in the player's status part 1: 2 = Zeni [2], 3 = Possible_ TP medals [4].
            // 1 = TP tokens (premium): never sold on this free server.
            var status = accounts.OwnStatus(user);
            var part1 = ObjectTree.List(status[1], 5);
            var slot = currency switch { 2 => 2, 3 => 4, _ => -1 };
            var owned = slot >= 0 ? ObjectTree.Int64(part1[slot]) : 0;
            if (slot < 0 || owned < price)
            {
                log.LogWarning("Shop payment refused: product {Product} costs {Price} of currency {Currency}, " +
                               "player has {Owned}", productId, price, currency, owned);
                return responses.Respond(new object?[] { RefusedStatus, 0, 0 });
            }

            part1[slot] = owned - price;
            await accounts.SaveStatusAsync(user, status, ct);

            var granted = new List<string>();
            foreach (var item in items)
                granted.Add(await GrantAsync(env, accounts, user, ObjectTree.Text(item[1]), ObjectTree.Int32(item[2]), ct));

            var reply = new object?[] { 0, 0, 0 };
            log.LogInformation("Shop payment: product {Product} ({Name}) for {Price} of currency {Currency}: " +
                               "{Before} -> {After}; {Granted}; args={Args}", productId, ObjectTree.Text(product[2]),
                price, currency, owned, owned - price, string.Join(", ", granted), ObjectTree.ToJson(args));
            return responses.Respond(reply);
        });

        return app;
    }

    // Reply status for a refused purchase. Possible_: any non-zero status the game knows; the right "not enough
    // currency" code is not known yet (the game checks the price itself before it asks).
    private const int RefusedStatus = 1;

    private static List<object?>? FindProduct(List<Dictionary<string, object?>> categories, long productId)
    {
        foreach (var category in categories)
        foreach (var entry in ObjectTree.List(Get(category, "lineup") ?? new List<object?>()))
        {
            var product = ObjectTree.List(entry, 14);
            var items = ObjectTree.List(product[13]);
            var firstItemId = items.Count > 0 ? ObjectTree.Int64(ObjectTree.List(items[0], 4)[0]) : -1;
            if (ObjectTree.Int64(product[1]) == productId || firstItemId == productId)
                return product;
        }
        return null;
    }

    // Adds one bought item to the player's save. Returns a short note for the log.
    private static async Task<string> GrantAsync(IWebHostEnvironment env, AccountService accounts,
        Data.PatrollerUser user, string type, int id, CancellationToken ct)
    {
        switch (type)
        {
            case "RivalID":
            {
                var reply = ObjectTree.List(accounts.Rivals(user), 2);
                var list = ObjectTree.List(reply[1]);
                if (list.Any(r => ObjectTree.Int32(ObjectTree.List(r)[0]) == id))
                    return $"Raider {id} already owned";
                var template = RivalTemplate(env, id);
                if (template is null)
                    return $"Raider {id} NOT added (no template in rival-templates.json)";
                list.Add(template);
                await accounts.SaveRivalsAsync(user, reply, ct);
                return $"Raider {id} added";
            }
            case "CostumeItemID":
            {
                // character/get_item part 1 = owned costumes [[costume id, "yyyy-MM-dd HH:mm:ss"], ...]
                var reply = ObjectTree.List(accounts.Items(user), 2);
                var list = ObjectTree.List(reply[1]);
                if (list.Any(c => ObjectTree.Int32(ObjectTree.List(c)[0]) == id))
                    return $"costume {id} already owned";
                list.Add(new List<object?> { id, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") });
                await accounts.SaveItemsAsync(user, reply, ct);
                return $"costume {id} added";
            }
            default:
                return $"{type} {id} NOT added (item type not supported yet)";
        }
    }

    private static object? RivalTemplate(IWebHostEnvironment env, int id)
    {
        var path = Path.Combine(env.ContentRootPath, "Assets", "rival-templates.json");
        if (!File.Exists(path))
            return null;
        var root = ObjectTree.FromJson(File.ReadAllText(path)) as Dictionary<string, object?>;
        if (root?.GetValueOrDefault("rivals") is not Dictionary<string, object?> rivals)
            return null;
        return rivals.TryGetValue(id.ToString(), out var entry) ? entry : null;
    }

    // [header, args] -> args (empty list when there are none).
    private static async Task<List<object?>> ReadArgsAsync(HttpRequest request, CancellationToken ct) =>
        ArgsOf(await MessagePackHttp.ReadAsync(request, ct));

    private static List<object?> ArgsOf(object? body)
    {
        var list = ObjectTree.List(body);
        return list.Count > 1 && list[1] is not null ? ObjectTree.List(list[1]) : new List<object?>();
    }

    private static List<Dictionary<string, object?>> Categories(IWebHostEnvironment env)
    {
        var path = Path.Combine(env.ContentRootPath, "Assets", "shop.json");
        if (!File.Exists(path))
            return new List<Dictionary<string, object?>>();
        var root = ObjectTree.FromJson(File.ReadAllText(path)) as Dictionary<string, object?>;
        if (root is null || !root.TryGetValue("categories", out var list) || list is null)
            return new List<Dictionary<string, object?>>();
        return ObjectTree.List(list).OfType<Dictionary<string, object?>>().ToList();
    }

    private static object? Get(Dictionary<string, object?> map, string key) =>
        map.TryGetValue(key, out var value) ? value : null;

    private static bool IsNumber(object? value) => value is int or long or uint or ulong or short or ushort or byte or sbyte;
}
