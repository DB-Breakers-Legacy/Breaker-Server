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
    private const int AdjustmentDataVersion = 2;

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
        Static("/avatar_create/get_list", "getAvatarCreateList");
        Static("/user/get_ban_status", "getBanStatus");

        // Each player's own level / rank / season pass (server-wide parts from getPatrollerStatus) and owned items.
        api.MapPost("/patroller/get_status", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);
            return responses.Respond(accounts.Status(user));
        });

        api.MapPost("/character/get_item", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);
            return responses.Respond(accounts.Items(user));
        });
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

            return responses.Respond(new object?[] { 0, AdjustmentDataVersion, text });
        });

        // Battle-data version check before matching (ServerApiResponseGetOnlineBattleDataVersion,
        // parser 0x1407F0C00: [status, version]). Must match the version adjustment_data_manage/read
        // returns (its parser 0x1407F02B0 reads [status, version, data] with the same field reader).
        // Missing, this 404 made the client show "a system error occurred when connecting to the
        // matchmaking server" (live test 2026-11-10).
        api.MapPost("/adjustment_data_manage/get_version", (ApiResponseService responses) =>
            responses.Respond(new object?[] { 0, AdjustmentDataVersion }));

        // Stages the player may pick before matching (ServerApiResponseGetSelectableBattleStage, parser
        // 0x1407F4FF0 -> [status, list]; list reader 0x1407C7A80: array of [string, string]). Join step 17
        // (sub_140816D90) keeps entries whose first string is all digits (the stage number, atoi) with the
        // second string (Possible_ display name or period). Non-numeric entries are skipped and an empty list
        // is accepted (the player then sends desiredStageId "unselected"). Missing, the 404 made practice
        // show "failed to connect" (live test 2026-11-10). Empty until we know the real stage numbers.
        api.MapPost("/season/get_selectable_stage", (ApiResponseService responses) =>
            responses.Respond(new object?[] { 0, Array.Empty<object?>() }));

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
            var raider = rows.Select(v => ObjectTree.List(v))
                .FirstOrDefault(x => ObjectTree.Int32(x[0]) == requested)
                ?? throw new InvalidDataException("The requested rival does not exist.");

            var points = ObjectTree.Int32(raider[3]);
            foreach (var item in ObjectTree.List(raider[4]).Select(v => ObjectTree.List(v)))
            {
                points += ObjectTree.Int32(item[1]);
                item[1] = 0;
            }

            foreach (var item in ObjectTree.List(raider[5]).Select(v => ObjectTree.List(v)))
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
                var raider = rows.Select(v => ObjectTree.List(v))
                    .FirstOrDefault(x => ObjectTree.Int32(x[0]) == raiderId)
                    ?? throw new InvalidDataException("The requested rival does not exist.");
                var left = ObjectTree.Int32(raider[3]) - newLevel;

                if (left < 0)
                    break;

                raider[3] = left;
                var skills = ObjectTree.List(raider[4]).Select(v => ObjectTree.List(v)).ToList();
                if (!skills.Any(x => ObjectTree.Int32(x[0]) == skillId))
                    skills = ObjectTree.List(raider[5]).Select(v => ObjectTree.List(v)).ToList();

                var target = skills.FirstOrDefault(x => ObjectTree.Int32(x[0]) == skillId);
                if (target is not null)
                    target[1] = newLevel;
            }

            await accounts.SaveRivalsAsync(user, rivals, ct);
            var resultRows = rows
                .Select(v => ObjectTree.List(v))
                .Select(x => new object?[] { x[0], x[3], x[4], x[5] })
                .ToList();

            return responses.Respond(new object?[] { 0, resultRows });
        });

        // Survivor skill training (ServerApiRequestExecuteSkillTraining). Official traffic (tutorial capture
        // 2026-10-16): request args [[[skillId, level], ...]], reply [0, [[skillId, level, 1], ...], 0]. The reply
        // reader sub_1407E1C40 reads [status, list of [skillId, level, flag] (sub_1407CA3A0), int]. The official
        // server keeps the levels in character/get part 3 as [skillId, level, flag, date]: untouched skills are
        // level 0 flag 0, trained ones level 20 (or 1 for one-level skills) flag 1. Free server: no cost taken.
        api.MapPost("/skill/training", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var bodyObj = await MessagePackHttp.ReadAsync(request, ct);
            var body = ObjectTree.List(bodyObj, 2);
            var user = await accounts.GetAsync(identity.ResolveUserId(bodyObj), ct);
            var characters = ObjectTree.List(accounts.Characters(user));
            var skills = ObjectTree.List(characters[3]);
            var trained = new List<object?>();

            foreach (var pair in ObjectTree.List(ObjectTree.List(body[1], 1)[0]).Select(x => ObjectTree.List(x, 2)))
            {
                var skillId = ObjectTree.Int32(pair[0]);
                var level = ObjectTree.Int32(pair[1]);
                var entry = skills.Select(v => ObjectTree.List(v)).FirstOrDefault(x => ObjectTree.Int32(x[0]) == skillId);
                if (entry is null)
                {
                    entry = new List<object?> { skillId, level, 1, DateTimeOffset.Now.ToString("yyyy-MM-dd HH:mm:ss") };
                    skills.Add(entry);
                }
                else
                {
                    entry[1] = level;
                    entry[2] = 1;
                }

                trained.Add(new object?[] { skillId, level, 1 });
            }

            await accounts.SaveCharactersAsync(user, characters, ct);
            return responses.Respond(new object?[] { 0, trained, 0 });
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
            // Fresh key set per session (as the official server and the Python reference do), registered so
            // the matching UDP listener can find it by SID. Wire order: SID, AES key, IV, HMAC key.
            // If all four Diarkis keys are configured, that fixed set is used instead (development override).
            var keys = options.Value.HasFixedDiarkisKeys
                ? registry.Register(new SessionKeys(
                    options.Value.DiarkisSidKey,
                    options.Value.DiarkisAesKey,
                    options.Value.DiarkisIvKey,
                    options.Value.DiarkisHashKey))
                : registry.Create();
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
            var target = rows.Select(v => ObjectTree.List(v))
                .FirstOrDefault(x => ObjectTree.Int32(x[0]) == characterId)
                ?? throw new InvalidDataException("The requested character does not exist.");

            ObjectTree.List(target[4]).Add(new object?[]
            {
                attackId,
                DateTimeOffset.Now.ToString("yyyy-MM-dd HH:mm:ss")
            });

            await accounts.SaveCharactersAsync(user, characters, ct);

            var result = rows
                .Select(v => ObjectTree.List(v))
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
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct), 2);
            var args = ObjectTree.List(body[1], 2);
            var roster = ObjectTree.List(args[1], 1);
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

        // Practice battle start (ServerApiResponsePracticeBattleStart, parser 0x1407DD9E0): [status, string]
        // (string reader sub_1407CFF90; Possible_ the battle id, like battle/start's second field). Request
        // args: [[userId, ...]] (live: just the player). Missing, the 404 ended practice right after the
        // connection room was set up (client error 10074141877067; live 2026-11-10).
        api.MapPost("/battle/practice_start", async (
            HttpRequest request,
            ApiResponseService responses,
            CancellationToken ct) =>
        {
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct), 2);
            var args = ObjectTree.List(body[1], 1);
            var roster = ObjectTree.List(args[0], 1);
            var leader = ObjectTree.Text(roster[0]);
            return responses.Respond(new object?[] { 0, $"{leader}_practice_{DateTimeOffset.Now:yyyyMMddHHmmss}" });
        });

        // Practice battle result (ServerApiResponsePracticeBattleResult, parser 0x1407DA7B0): [status] only.
        api.MapPost("/battle/practice_result", (ApiResponseService responses) =>
            responses.Respond(new object?[] { 0 }));

        // ---- Calls the official server answers that we had no route for (official api reference\
        // SERVER_CHECKLIST.md, 2026-11-12). Shapes copied from the official replies in the user's captures. ----

        // Plain acknowledgements: the official reply is [0]. Free community server: reports, blocks and friend
        // requests are accepted but not stored yet (no other players' data is kept).
        foreach (var path in new[]
                 {
                     "/regularly_run/regularly_maintenance_check", // args [] -> [0]
                     "/user/update_orientation_step",              // args [step] -> [0] (tutorial: [0] at the end)
                     "/report/report",                             // args [int, text] -> [0]
                     "/block/add_block_user",                      // args [[userId]] -> [0]
                     "/friend/send_friend_request"                 // args [userId] -> [0]
                 })
            api.MapPost(path, (ApiResponseService responses) => responses.Respond(new object?[] { 0 }));

        // Friend list: args [page, perPage, type] (official [1, 100, 0]); reply [0, page, total, entries]. Empty
        // until we have our own friends feature (official empty reply: [0, 1, 0, []]).
        api.MapPost("/friend/get_friend_list", async (HttpRequest request, ApiResponseService responses, CancellationToken ct) =>
        {
            var page = 1;
            try
            {
                var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct), 2);
                page = ObjectTree.Int32(ObjectTree.List(body[1], 1)[0]);
            }
            catch (Exception)
            {
                // Unreadable request: answer page 1.
            }

            return responses.Respond(new object?[] { 0, page, 0, Array.Empty<object?>() });
        });

        // Shop with real money (TP packs at USD prices on the official server). Free server: nothing is sold, so
        // the catalog is empty. Official reply [0, [[id, name, name, "", price, price, json], ...]].
        api.MapPost("/commonpurchase/get_catalog", (ApiResponseService responses) =>
            responses.Respond(new object?[] { 0, Array.Empty<object?>() }));

        // Legal payment notice (official: a long Japanese text). args [language] -> [0, text].
        api.MapPost("/commonpurchase/get_fund_settlement_text", (ApiResponseService responses) =>
            responses.Respond(new object?[] { 0, FreeServerNotice }));

        // Owned costumes: args [] -> [0, [[costumeId, date], ...]]; the official list equals character/get_item
        // part 1 (same ids and dates), so the same part of the player's own item reply is returned.
        api.MapPost("/costume/get_list", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);
            return responses.Respond(new object?[] { 0, ObjectTree.List(accounts.Items(user), 2)[1] });
        });

        // Survivor list for the transformation-ball screen: args [] -> [0, rows]; the official rows equal
        // character/get part 1, so the player's own saved rows are returned.
        api.MapPost("/transball/get_list", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            AccountService accounts,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var user = await accounts.GetAsync(identity.ResolveUserId(body), ct);
            return responses.Respond(new object?[] { 0, ObjectTree.List(accounts.Characters(user), 2)[1] });
        });

        // Tutorial ("orientation") battles 1-3. Start: args [stage] -> [0, "<userId>_<yyyyMMddHHmmss>"] (official
        // id format). Only reached when user/create_user_info's third value is 1; ours sends 0 (tutorial skipped).
        api.MapPost("/battle/orientation_start", async (
            HttpRequest request,
            ApiResponseService responses,
            RequestIdentityResolver identity,
            CancellationToken ct) =>
        {
            var body = await MessagePackHttp.ReadAsync(request, ct);
            var userId = identity.ResolveUserId(body);
            return responses.Respond(new object?[] { 0, $"{userId}_{DateTimeOffset.Now:yyyyMMddHHmmss}" });
        });

        // Tutorial result: args [battleId, stage, 128-char proof]. Reader sub_1407FAE70: [status, 5 ints
        // (sub_1407CC8D0), list of [category, points] (sub_1407C7F70), list of 4-int entries (sub_1407CDA70, the
        // same reader as battle/result field 8, whose missing Raider entry crashed the result screen on
        // 2026-11-11)]. Meaning of the 5 ints not known yet, so the official reply for each stage is sent back
        // unchanged (stage 2 is played as Cell = Raider 100 and carries his entry).
        api.MapPost("/battle/orientation_result", async (HttpRequest request, ApiResponseService responses, CancellationToken ct) =>
        {
            var stage = 1;
            try
            {
                var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct), 2);
                stage = ObjectTree.Int32(ObjectTree.List(body[1], 2)[1]);
            }
            catch (Exception)
            {
                // Unreadable request: answer as stage 1.
            }

            return responses.Respond(OrientationResult(stage));
        });

        // Battle result (ServerApiResponseBattleResult, parser 0x1407DB070, fields 0..10).
        // Field 8 (parser sub_1407CDA70 -> reply +0x240/+0x248) is a list of [raiderId, rankIndex, value, value]
        // (16-byte entries). When a normal match ends with the player as the Raider, the result screen
        // (sub_14043ABF0, at 0x14043B0C4) looks up the player slot's Raider id (slot +0x11C) in it and reads the
        // entry without checking it was found: with an empty list the game crashed (live 2026-11-11, 0x14043B0E2,
        // null read; Raider = Cell, id 100). The request carries that id as argument 2 (live: [battleId, 1, 100,
        // 2, ...]), so one entry for it is returned, rank row 0 and no gains.
        api.MapPost("/battle/result", async (HttpRequest request, ApiResponseService responses, CancellationToken ct) =>
        {
            var raiderEntries = new List<object?>();
            try
            {
                var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct), 2);
                var args = ObjectTree.List(body[1], 3);
                raiderEntries.Add(new object?[] { ObjectTree.Int32(args[2]), 0, 0, 0 });
            }
            catch (Exception)
            {
                // Unreadable request: keep the list empty (survivor results never read it).
            }

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
                raiderEntries.ToArray(),
                1.0,
                new object?[] { 0, 0 }
            });
        });
    }

    // Official tutorial results (breakers tutorial.pcapng, 2026-10-16), one per stage.
    private static object?[] OrientationResult(int stage) => stage switch
    {
        2 => new object?[]
        {
            0,
            new object?[] { 2, 50, 4400, 5360, 0 },
            new object?[]
            {
                new object?[] { "category_broke_super_timemachine", 200 },
                new object?[] { "category_evolution", 100 },
                new object?[] { "category_move", 0 },
                new object?[] { "category_result", 200 },
                new object?[] { "category_survivor", 150 }
            },
            new object?[] { new object?[] { 100, 2, 200, 1 } }
        },
        3 => new object?[]
        {
            0,
            new object?[] { 3, 190, 6700, 5500, 0 },
            new object?[]
            {
                new object?[] { "category_interact_system", 150 },
                new object?[] { "category_move", 100 },
                new object?[] { "category_raider_decoy", 0 },
                new object?[] { "category_result", 200 },
                new object?[] { "category_scramble", 150 }
            },
            Array.Empty<object?>()
        },
        _ => new object?[]
        {
            0,
            new object?[] { 2, 50, 1800, 5205, 0 },
            new object?[]
            {
                new object?[] { "category_alive", 100 },
                new object?[] { "category_discover_key", 0 },
                new object?[] { "category_interact_system", 150 },
                new object?[] { "category_move", 100 },
                new object?[] { "category_result", 150 }
            },
            Array.Empty<object?>()
        }
    };

    private const string FreeServerNotice =
        "DB Breakers Legacy is a free community server. Nothing is sold and no payment is taken.";

    private const string TokushoText =
        "This notification based on Japan's Act on Specified Commercial Transactions (Act No. 57 of 1976) " +
        "is intended for customers residing in Japan.\n\n[Publisher legal notice unavailable.]";
}
