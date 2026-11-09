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
        MapGame(app, "/dbtb-prd/{title}/api");
        MapGame(app, "/dbtb-prd/{prefix}/{title}/api");
    }

    private static void MapGame(IEndpointRouteBuilder app, string prefix)
    {
        var api = app.MapGroup(prefix);
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

        api.MapPost("/battle/consume_priority_point", async (
            HttpRequest request,
            ApiResponseService responses,
            ResponseCatalog catalog,
            MatchmakingCoordinator coordinator,
            RequestIdentityResolver identity,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            coordinator.RequireBattle(identity.ResolveUserId(body).ToString());
            return responses.Respond(catalog.Get("consumePriorityPoint"));
        });

        api.MapPost("/battle/get_battle_member_result_list", async (
            HttpRequest request,
            ApiResponseService responses,
            ResponseCatalog catalog,
            MatchmakingCoordinator coordinator,
            RequestIdentityResolver identity,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            coordinator.RequireBattle(identity.ResolveUserId(body).ToString());
            return responses.Respond(catalog.Get("getBattleMemberResultList"));
        });

        api.MapPost("/adjustment_data_manage/get_version", (ApiResponseService responses) =>
            responses.Respond(new object?[] { 0, 2 }));

        api.MapPost("/season/get_selectable_stage", (ApiResponseService responses) =>
            responses.Respond(new object?[]
            {
                0,
                new object?[]
                {
                    new object?[] { "1000", "ハイランドリバー" },
                    new object?[] { "1100", "スノーマウンテン" },
                    new object?[] { "1110", "ロックフィールド" },
                    new object?[] { "1200", "ウォーターフロント" },
                    new object?[] { "1300", "グリーンコースト" },
                    new object?[] { "1400", "デンジャーグラウンド" },
                    new object?[] { "1500", "ダークフューチャー" }
                }
            }));

        api.MapPost("/user/create_user_info", async (
            HttpRequest request,
            ApiResponseService responses,
            CancellationToken ct) =>
        {
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct), 2);
            var args = ObjectTree.List(body[1], 1);
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
            var body = ObjectTree.List(bodyObj, 2);
            var user = await accounts.GetAsync(identity.ResolveUserId(bodyObj), ct);
            var requested = ObjectTree.Int32(ObjectTree.List(body[1], 1)[0]);
            var rivals = ObjectTree.List(accounts.Rivals(user));
            var rows = ObjectTree.List(rivals[1]);
            var raider = rows.Select(ObjectTree.List)
                .FirstOrDefault(x => ObjectTree.Int32(x[0]) == requested)
                ?? throw new InvalidDataException("The requested rival does not exist.");

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
            var body = ObjectTree.List(bodyObj, 2);
            var user = await accounts.GetAsync(identity.ResolveUserId(bodyObj), ct);
            var rivals = ObjectTree.List(accounts.Rivals(user));
            var rows = ObjectTree.List(rivals[1]);

            foreach (var train in ObjectTree.List(ObjectTree.List(body[1], 1)[0]).Select(x => ObjectTree.List(x, 3)))
            {
                var raiderId = ObjectTree.Int32(train[0]);
                var skillId = ObjectTree.Int32(train[1]);
                var newLevel = ObjectTree.Int32(train[2]);
                var raider = rows.Select(ObjectTree.List)
                    .FirstOrDefault(x => ObjectTree.Int32(x[0]) == raiderId)
                    ?? throw new InvalidDataException("The requested rival does not exist.");
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
                    options.Value.UdpPort,
                    keys.Sid,
                    keys.Aes,
                    keys.Iv,
                    keys.Mac
                },
                new object?[] { 1, 1 }
            });
        });

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
            RequestIdentityResolver identity,
            BreakerDbContext db,
            CancellationToken ct) =>
        {
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct), 2);
            identity.ResolveUserId(body);
            var id = ObjectTree.Text(ObjectTree.List(body[1], 3)[2]);
            var message = await db.MessageBots.SingleOrDefaultAsync(x => x.MessageId == id, ct)
                ?? throw new InvalidDataException("The requested message does not exist.");
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
            var body = ObjectTree.List(bodyObj, 2);
            var pair = ObjectTree.List(ObjectTree.List(ObjectTree.List(body[1], 1)[0], 1)[0], 2);
            var characterId = ObjectTree.Int32(pair[0]);
            var attackId = ObjectTree.Int32(pair[1]);
            var user = await accounts.GetAsync(identity.ResolveUserId(bodyObj), ct);
            var characters = ObjectTree.List(accounts.Characters(user));
            var rows = ObjectTree.List(characters[1]);
            var target = rows.Select(ObjectTree.List)
                .FirstOrDefault(x => ObjectTree.Int32(x[0]) == characterId)
                ?? throw new InvalidDataException("The requested character does not exist.");

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

        api.MapPost("/battle/get_connection_server_info", async (
            HttpRequest request,
            ApiResponseService responses,
            SessionKeyRegistry registry,
            MatchmakingCoordinator coordinator,
            RequestIdentityResolver identity,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var binding = coordinator.GetConnectionBinding(identity.ResolveUserId(body).ToString());
            var keys = registry.Create();
            return responses.Respond(new object?[]
            {
                0,
                new object?[]
                {
                    binding.Host,
                    binding.Port,
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
            MatchmakingCoordinator coordinator,
            RequestIdentityResolver identity,
            CancellationToken ct) =>
        {
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct), 2);
            var userId = identity.ResolveUserId(body).ToString();
            var args = ObjectTree.List(body[1], 2);
            var roster = ObjectTree.List(args[1], 1);
            var leader = ObjectTree.Text(roster[0]);
            var battleId = coordinator.StartBattle(userId, leader);

            return responses.Respond(new object?[]
            {
                0,
                battleId,
                new object?[] { "", 0, 0, 0, 0 },
                Array.Empty<object?>()
            });
        });

        api.MapPost("/battle/result", async (
            HttpRequest request,
            ApiResponseService responses,
            MatchmakingCoordinator coordinator,
            RequestIdentityResolver identity,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            coordinator.CompleteBattle(identity.ResolveUserId(body).ToString());
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
