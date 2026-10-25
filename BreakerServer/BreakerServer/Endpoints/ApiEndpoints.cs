using BreakerServer.Authentication;
using BreakerServer.Configuration;
using BreakerServer.Data;
using BreakerServer.Infrastructure;
using BreakerServer.Services;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Options;

namespace BreakerServer.Endpoints;

public static class ApiEndpoints
{
    public static IEndpointRouteBuilder MapBreakerApi(this IEndpointRouteBuilder app)
    {
        app.MapBootstrapEndpoints();
        app.MapAuthEndpoints();
        app.MapNameValidationEndpoints();
        app.MapNewsEndpoints();
        MapGame(app);
        return app;
    }

    private static void MapGame(IEndpointRouteBuilder app)
    {
        var api = app.MapGroup("/dbtb-prd/{title}/api");
        void Static(string path, string name) =>
            api.MapPost(path, (ApiResponseService responses, ResponseCatalog catalog) =>
                responses.Respond(catalog.Get(name)));

        Static("/sys/kpi", "envKPI");
        Static("/close/get_close_info", "getCloseInfo");
        Static("/patroller/get_status", "getPatrollerStatus");
        Static("/avatar_create/get_list", "getAvatarCreateList");
        Static("/character/get_item", "getCharacterItems");
        Static("/user/get_ban_status", "getBanStatus");
        Static("/commonpurchase/get_purchase_status", "getPurchaseStatus");
        Static("/item/item_possession", "getItemPossession");
        Static("/battle/get_challenge_list", "getChallengeList");
        Static("/event/get_schedule_list", "getScheduleList");
        Static("/user/update_manner_point", "updateMannerPoint");
        Static("/bnid_reward/grant_reward", "grantReward");
        Static("/message/update_message_item_received", "updateMessageList");
        Static("/regularly_run/get_emergency_message", "getEmergencyMessage");
        Static("/lootbox/ticket_master_list", "ticketMasterList");
        Static("/leaderboard/get_leaderboard_model", "getLeaderboardModel");
        Static("/user/update_avatar", "updateAvatar");
        Static("/skill/training", "skillTraining");
        Static("/battle/waittime_preview", "waittimePreview");
        Static("/battle/pre_matching_connection", "preMatchingConnection");
        Static("/battle/save_matching_cache", "saveMatchingCache");
        Static("/player/upload_ghost_player", "uploadGhostPlayer");
        Static("/battle/consume_priority_point", "consumePriorityPoint");
        Static("/battle/get_battle_member_result_list", "getBattleMemberResultList");

        api.MapPost("/user/create_user_info", async (
            HttpRequest request,
            ApiResponseService responses,
            CancellationToken ct) =>
        {
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct));
            var args = ObjectTree.List(body[1]);
            return responses.Respond(new object?[] { 0, ObjectTree.Text(args[0]), 0 });
        });

        api.MapPost("/adjustment_data_manage/read", (
            ApiResponseService responses,
            IOptions<BreakerOptions> options,
            ILoggerFactory logs) =>
        {
            var path = options.Value.AdjustmentDataPath;
            var text = string.Empty;

            if (!string.IsNullOrWhiteSpace(path))
            {
                try
                {
                    text = File.ReadAllText(path);
                }
                catch (FileNotFoundException)
                {
                    logs.CreateLogger("AdjustmentData").LogWarning(
                        "Adjustment data missing: {Path}",
                        path);
                }
            }

            return responses.Respond(new object?[] { 0, 2, text });
        });

        api.MapPost("/battle/get_stun_server_info", (
            ApiResponseService responses,
            IOptions<BreakerOptions> options) =>
            responses.Respond(new object?[]
            {
                0,
                options.Value.StunPorts
                    .Select(port => new object?[] { options.Value.StunHost, port })
                    .ToList()
            }));

        api.MapPost("/gamecurrency/get_owned", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);
            return responses.Respond(accounts.Currency(user));
        });

        api.MapPost("/rival/get_list", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);
            return responses.Respond(accounts.Rivals(user));
        });

        api.MapPost("/rival/skill_reset", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var bodyObj = await MessagePackHttp.ReadAsync(request, ct);
            var body = ObjectTree.List(bodyObj);
            var user = await accounts.GetAsync(identity.ResolveUserId(bodyObj), ct);
            var requested = ObjectTree.Int32(ObjectTree.List(body[1])[0]);
            var rivals = ObjectTree.List(accounts.Rivals(user));
            var rows = ObjectTree.List(rivals[1]);
            var raider = rows.Select(ObjectTree.List)
                .First(x => ObjectTree.Int32(x[0]) == requested);

            var points = ObjectTree.Int32(raider[3]);
            foreach (var item in ObjectTree.List(raider[4]).Select(ObjectTree.List))
            {
                points += ObjectTree.Int32(item[1]);
                item[1] = 0;
            }

            foreach (var item in ObjectTree.List(raider[5]).Select(ObjectTree.List))
            {
                points += ObjectTree.Int32(item[1]);
                item[1] = 0;
            }

            raider[3] = points;
            await accounts.SaveRivalsAsync(user, rivals, ct);

            return responses.Respond(new object?[]
            {
                0,
                new object?[] { requested, points, raider[4], raider[5] }
            });
        });

        api.MapPost("/rival/training", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var bodyObj = await MessagePackHttp.ReadAsync(request, ct);
            var body = ObjectTree.List(bodyObj);
            var user = await accounts.GetAsync(identity.ResolveUserId(bodyObj), ct);
            var rivals = ObjectTree.List(accounts.Rivals(user));
            var rows = ObjectTree.List(rivals[1]);

            foreach (var train in ObjectTree.List(ObjectTree.List(body[1])[0]).Select(ObjectTree.List))
            {
                var raiderId = ObjectTree.Int32(train[0]);
                var skillId = ObjectTree.Int32(train[1]);
                var newLevel = ObjectTree.Int32(train[2]);
                var raider = rows.Select(ObjectTree.List)
                    .First(x => ObjectTree.Int32(x[0]) == raiderId);
                var left = ObjectTree.Int32(raider[3]) - newLevel;

                if (left < 0)
                    break;

                raider[3] = left;
                var skills = ObjectTree.List(raider[4]).Select(ObjectTree.List).ToList();
                if (!skills.Any(x => ObjectTree.Int32(x[0]) == skillId))
                    skills = ObjectTree.List(raider[5]).Select(ObjectTree.List).ToList();

                var target = skills.FirstOrDefault(x => ObjectTree.Int32(x[0]) == skillId);
                if (target is not null)
                    target[1] = newLevel;
            }

            await accounts.SaveRivalsAsync(user, rivals, ct);
            var resultRows = rows
                .Select(ObjectTree.List)
                .Select(x => new object?[] { x[0], x[3], x[4], x[5] })
                .ToList();

            return responses.Respond(new object?[] { 0, resultRows });
        });

        api.MapPost("/character/get", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);
            return responses.Respond(accounts.Characters(user));
        });

        api.MapPost("/battle/get_diarkis_matching_server_info", (
            ApiResponseService responses,
            IOptions<BreakerOptions> options) =>
            responses.Respond(new object?[]
            {
                0,
                new object?[]
                {
                    options.Value.UdpHost,
                    options.Value.UdpPort,
                    options.Value.DiarkisIvKey.ToLowerInvariant(),
                    options.Value.DiarkisAesKey.ToLowerInvariant(),
                    options.Value.DiarkisSidKey.ToLowerInvariant(),
                    options.Value.DiarkisHashKey.ToLowerInvariant()
                },
                new object?[] { 1, 1 }
            }));

        api.MapPost("/message/get_message_list", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);
            return responses.Respond(accounts.Messages(user));
        });

        api.MapPost("/message/get_message_info", async (
            HttpRequest request,
            ApiResponseService responses,
            BreakerDbContext db,
            CancellationToken ct) =>
        {
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct));
            var id = ObjectTree.Text(ObjectTree.List(body[1])[2]);
            var message = await db.MessageBots.SingleAsync(x => x.MessageId == id, ct);
            return responses.Respond(ObjectTree.FromJson(message.MessageContentsJson));
        });

        api.MapPost("/commonpurchase/tokusho/", (ApiResponseService responses) =>
            responses.Respond(new object?[] { 0, TokushoText }));

        api.MapPost("/transball/unlock_spattack", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var bodyObj = await MessagePackHttp.ReadAsync(request, ct);
            var body = ObjectTree.List(bodyObj);
            var pair = ObjectTree.List(ObjectTree.List(ObjectTree.List(body[1])[0])[0]);
            var characterId = ObjectTree.Int32(pair[0]);
            var attackId = ObjectTree.Int32(pair[1]);
            var user = await accounts.GetAsync(identity.ResolveUserId(bodyObj), ct);
            var characters = ObjectTree.List(accounts.Characters(user));
            var rows = ObjectTree.List(characters[1]);
            var target = rows.Select(ObjectTree.List)
                .First(x => ObjectTree.Int32(x[0]) == characterId);

            ObjectTree.List(target[4]).Add(new object?[]
            {
                attackId,
                DateTimeOffset.Now.ToString("yyyy-MM-dd HH:mm:ss")
            });

            await accounts.SaveCharactersAsync(user, characters, ct);

            var result = rows
                .Select(ObjectTree.List)
                .Select(x => new object?[]
                {
                    x[0],
                    ObjectTree.List(x[4])
                        .Select(y => ObjectTree.List(y)[0])
                        .ToList()
                })
                .ToList();

            return responses.Respond(new object?[] { 0, result, 696969 });
        });

        api.MapPost("/battle/get_connection_server_info", (
            ApiResponseService responses,
            SessionKeyRegistry registry,
            IOptions<BreakerOptions> options) =>
        {
            var keys = registry.Create();
            return responses.Respond(new object?[]
            {
                0,
                new object?[]
                {
                    options.Value.UdpHost,
                    options.Value.UdpSessionPort,
                    keys.Sid,
                    keys.Aes,
                    keys.Iv,
                    keys.Mac
                }
            });
        });

        api.MapPost("/battle/start", async (
            HttpRequest request,
            ApiResponseService responses,
            CancellationToken ct) =>
        {
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct));
            var args = ObjectTree.List(body[1]);
            var roster = ObjectTree.List(args[1]);
            var leader = ObjectTree.Text(roster[0]);
            var battleId = $"{leader}_{DateTimeOffset.Now:yyyyMMddHHmmss}";

            return responses.Respond(new object?[]
            {
                0,
                battleId,
                new object?[] { "", 0, 0, 0, 0 },
                Array.Empty<object?>()
            });
        });

        api.MapPost("/battle/result", (ApiResponseService responses) =>
        {
            object?[] pass =
            [
                9,
                "2025-07-30 02:00:00",
                "2050-12-31 14:59:59",
                9,
                0,
                Array.Empty<object?>(),
                35,
                Array.Empty<object?>(),
                new object?[] { "", "" }
            ];

            return responses.Respond(new object?[]
            {
                0,
                new object?[] { 0, 0, 0, 0, 0 },
                new object?[] { 0, 0, 0, 0, 0, 0 },
                new object?[] { 0, 0, 0, 0 },
                pass,
                new object?[] { 1, 1, Array.Empty<object?>(), Array.Empty<object?>() },
                Array.Empty<object?>(),
                new object?[] { 0, 0, 0, 0, "2050-12-31 14:59:59" },
                Array.Empty<object?>(),
                1.0,
                new object?[] { 0, 0 }
            });
        });
    }

    private const string TokushoText =
        "This notification based on Japan's Act on Specified Commercial Transactions (Act No. 57 of 1976) " +
        "is intended for customers residing in Japan.\n\n[Publisher legal notice unavailable.]";
}
