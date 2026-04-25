using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Strings;

namespace SctBridge;

/// <summary>
/// Plugin I/O bridge — Mutagen-backed Skyrim SE implementation.
/// All methods are [UnmanagedCallersOnly]; caller manages buffer lifetime via sct_free_buffer.
///
/// State model:
///   s_sourcePlugin  — read-only overlay of the user-selected plugin (+ its declared masters)
///   s_linkCache     — covers s_sourcePlugin + its masters for full record resolution
///   s_projectMod    — writable mod; accumulates records created from SCT UI
/// </summary>
public static class PluginBridge
{
    [ThreadStatic]
    private static string? s_lastError;

    private static ISkyrimModGetter? s_sourcePlugin;
    private static ILinkCache?       s_linkCache;
    private static SkyrimMod?        s_projectMod;
    private static string            s_dataFolder = "";

    private static readonly JsonSerializerOptions s_json = new()
    {
        PropertyNamingPolicy   = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    // ── Source plugin ─────────────────────────────────────────────────────────
    //
    // Loads a plugin as a read-only binary overlay and builds a link cache
    // covering the plugin plus all masters it declares (Skyrim.esm, Update.esm,
    // DLC ESMs, etc.).  All downstream record resolution uses this cache.
    //
    // pluginPath   — absolute path to the .esp / .esm / .esl
    // dataFolder   — game Data directory (used to locate master files)
    // Returns 0 on success, -1 on failure (call sct_plugin_last_error for details).

    [UnmanagedCallersOnly(EntryPoint = "sct_plugin_load")]
    public static unsafe int PluginLoad(byte* pluginPathUtf8, byte* dataFolderUtf8)
    {
        try
        {
            var pluginPath = Marshal.PtrToStringUTF8((IntPtr)pluginPathUtf8)
                             ?? throw new ArgumentException("null plugin path");
            s_dataFolder   = Marshal.PtrToStringUTF8((IntPtr)dataFolderUtf8) ?? "";

            s_sourcePlugin = SkyrimMod.CreateFromBinaryOverlay(
                new ModPath(pluginPath), SkyrimRelease.SkyrimSE);

            // Load declared masters so link resolution can follow cross-plugin
            // references (e.g. NordRace lives in Skyrim.esm).
            var mods = new List<ISkyrimModGetter>();
            foreach (var master in s_sourcePlugin.MasterReferences)
            {
                var masterPath = Path.Combine(s_dataFolder, master.Master.FileName);
                if (!File.Exists(masterPath)) continue;
                mods.Add(SkyrimMod.CreateFromBinaryOverlay(
                    new ModPath(masterPath), SkyrimRelease.SkyrimSE));
            }
            mods.Add(s_sourcePlugin); // highest priority last
            s_linkCache = mods.ToUntypedImmutableLinkCache();

            s_lastError = null;
            return 0;
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    [UnmanagedCallersOnly(EntryPoint = "sct_plugin_unload")]
    public static void PluginUnload()
    {
        s_sourcePlugin = null;
        s_linkCache    = null;
    }

    // ── NPC search ────────────────────────────────────────────────────────────
    //
    // Searches the loaded source plugin for NPC_ records whose name or EditorID
    // contains the query string (case-insensitive).  Empty query returns up to
    // maxResults records from the start of the plugin.
    //
    // outJson / outLen — CoTaskMem-allocated UTF-8 JSON array; free via sct_free_buffer.
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_npc_search")]
    public static unsafe int NpcSearch(byte* queryUtf8, int maxResults,
                                        byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_sourcePlugin is null)
                throw new InvalidOperationException("no plugin loaded — call sct_plugin_load first");

            var query   = Marshal.PtrToStringUTF8((IntPtr)queryUtf8) ?? "";
            var cap     = maxResults > 0 ? maxResults : 100;
            var records = s_sourcePlugin.Npcs
                .Where(n => MatchesQuery(n, query))
                .Take(cap)
                .Select(ToNpcRecord)
                .ToList();

            return WriteJson(records, outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── NPC get by FormID ─────────────────────────────────────────────────────
    //
    // Fetches a single NPC_ record from the source plugin by its local FormID
    // (the 24-bit ID without the mod-index byte).
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_npc_get")]
    public static unsafe int NpcGet(uint formId, byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_sourcePlugin is null)
                throw new InvalidOperationException("no plugin loaded");

            var npc = s_sourcePlugin.Npcs.FirstOrDefault(n => n.FormKey.ID == formId)
                      ?? throw new KeyNotFoundException($"FormID {formId:X8} not found");

            return WriteJson(ToNpcRecord(npc), outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── Project mod ───────────────────────────────────────────────────────────
    //
    // SCT keeps a writable project mod separate from the read-only source plugin.
    // New NPC_ records created from the SCT UI accumulate here.

    [UnmanagedCallersOnly(EntryPoint = "sct_project_new")]
    public static unsafe int ProjectNew(byte* modNameUtf8)
    {
        try
        {
            var name = Marshal.PtrToStringUTF8((IntPtr)modNameUtf8)
                       ?? throw new ArgumentException("null mod name");
            s_projectMod = new SkyrimMod(
                ModKey.FromNameAndExtension(name), SkyrimRelease.SkyrimSE);
            s_lastError = null;
            return 0;
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    [UnmanagedCallersOnly(EntryPoint = "sct_project_load")]
    public static unsafe int ProjectLoad(byte* pathUtf8)
    {
        try
        {
            var path = Marshal.PtrToStringUTF8((IntPtr)pathUtf8)
                       ?? throw new ArgumentException("null path");
            s_projectMod = SkyrimMod.CreateFromBinary(
                new ModPath(path), SkyrimRelease.SkyrimSE);
            s_lastError = null;
            return 0;
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    [UnmanagedCallersOnly(EntryPoint = "sct_project_save")]
    public static unsafe int ProjectSave(byte* pathUtf8)
    {
        try
        {
            if (s_projectMod is null)
                throw new InvalidOperationException("no project loaded");
            var path = Marshal.PtrToStringUTF8((IntPtr)pathUtf8)
                       ?? throw new ArgumentException("null path");
            s_projectMod.WriteToBinary(path);
            s_lastError = null;
            return 0;
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── NPC create ────────────────────────────────────────────────────────────
    //
    // Creates a new NPC_ record in the project mod.
    //
    // inJson — UTF-8 JSON: { editorId, name, raceFormKey, isFemale }
    //   raceFormKey format: "RRRRRR:PluginName.esm"  (Mutagen FormKey string)
    //
    // outJson / outLen — the created record serialized as NpcRecord JSON.
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_npc_create")]
    public static unsafe int NpcCreate(byte* inJsonUtf8, byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_projectMod is null)
                throw new InvalidOperationException("no project loaded — call sct_project_new first");

            var json = Marshal.PtrToStringUTF8((IntPtr)inJsonUtf8)
                       ?? throw new ArgumentException("null JSON");
            var req  = JsonSerializer.Deserialize<NpcCreateRequest>(json, s_json)
                       ?? throw new ArgumentException("invalid JSON");

            var npc = s_projectMod.Npcs.AddNew(req.EditorId ?? "");

            if (req.Name is not null)
                npc.Name = new TranslatedString(Language.English, req.Name);

            if (req.RaceFormKey is not null && FormKey.TryFactory(req.RaceFormKey, out var raceKey))
                npc.Race.SetTo(raceKey);

            if (req.IsFemale)
                npc.Configuration.Flags |= NpcConfiguration.Flag.Female;

            return WriteJson(ToNpcRecord(npc), outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── Error retrieval ───────────────────────────────────────────────────────

    [UnmanagedCallersOnly(EntryPoint = "sct_plugin_last_error")]
    public static unsafe int LastError(byte* buf, int bufLen)
    {
        var msg   = s_lastError ?? "unknown error";
        var bytes = Encoding.UTF8.GetBytes(msg);
        int n     = Math.Min(bytes.Length, bufLen - 1);
        if (n > 0) bytes.AsSpan(0, n).CopyTo(new Span<byte>(buf, n));
        if (bufLen > 0) buf[n] = 0;
        return n;
    }

    // ── Internal helpers ──────────────────────────────────────────────────────

    private static bool MatchesQuery(INpcGetter npc, string query)
    {
        if (string.IsNullOrWhiteSpace(query)) return true;
        var q = query.Trim();
        return npc.Name?.String?.Contains(q, StringComparison.OrdinalIgnoreCase) == true
            || npc.EditorID?.Contains(q,    StringComparison.OrdinalIgnoreCase) == true;
    }

    private static NpcRecord ToNpcRecord(INpcGetter npc)
    {
        string? skeletonModelPath = null;
        string? expressionTriPath = null;
        string? raceEditorId      = null;

        if (s_linkCache is not null)
        {
            // Race → skeleton NIF path.
            // C++ uses this to derive creature type + HKX path via ExtractCreatureType().
            if (npc.Race.TryResolve(s_linkCache, out var race))
            {
                raceEditorId      = race.EditorID;
                var isFemale      = npc.Configuration.Flags.HasFlag(NpcConfiguration.Flag.Female);
                var skelModel     = isFemale ? race.SkeletalModel?.Female : race.SkeletalModel?.Male;
                skeletonModelPath = skelModel?.File.GivenPath;
            }

            // Head parts → expression TRI path.
            // HDPT Parts entries with PartType == Tri (NAM0=1) are the runtime expression TRI.
            foreach (var hpLink in npc.HeadParts)
            {
                if (!hpLink.TryResolve(s_linkCache, out var hp)) continue;
                foreach (var part in hp.Parts)
                {
                    if (part.PartType == Part.PartTypeEnum.Tri)
                    {
                        expressionTriPath = part.FileName?.GivenPath;
                        break;
                    }
                }
                if (expressionTriPath is not null) break;
            }
        }

        var pluginSource   = npc.FormKey.ModKey.FileName;
        var facegenNifPath = $"Meshes/Actors/Character/FaceGenData/FaceGeom/" +
                             $"{pluginSource}/{npc.FormKey.ID:X8}.nif";

        return new NpcRecord
        {
            FormId            = npc.FormKey.ID,
            FormKey           = npc.FormKey.ToString(),
            EditorId          = npc.EditorID,
            Name              = npc.Name?.String,
            RaceEditorId      = raceEditorId,
            IsFemale          = npc.Configuration.Flags.HasFlag(NpcConfiguration.Flag.Female),
            SkeletonModelPath = skeletonModelPath,
            ExpressionTriPath = expressionTriPath,
            FacegenNifPath    = facegenNifPath,
            PluginSource      = pluginSource,
        };
    }

    private static unsafe int WriteJson<T>(T value, byte** outJson, int* outLen)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(value, s_json);
        var ptr   = (byte*)Marshal.AllocCoTaskMem(bytes.Length + 1);
        bytes.CopyTo(new Span<byte>(ptr, bytes.Length));
        ptr[bytes.Length] = 0;
        *outJson    = ptr;
        *outLen     = bytes.Length;
        s_lastError = null;
        return 0;
    }

    // ── DTOs ──────────────────────────────────────────────────────────────────

    private record NpcRecord
    {
        public uint    FormId            { get; init; }
        public string? FormKey           { get; init; }
        public string? EditorId          { get; init; }
        public string? Name              { get; init; }
        public string? RaceEditorId      { get; init; }
        public bool    IsFemale          { get; init; }
        // Skeleton NIF path from the race record.
        // C++ derives creature type and HKX path via ExtractCreatureType().
        public string? SkeletonModelPath { get; init; }
        // Expression TRI path from head part (HDPT Part.PartTypeEnum.Tri).
        // Null if unresolvable (custom race not in loaded masters).
        public string? ExpressionTriPath { get; init; }
        // Derived: Meshes/Actors/Character/FaceGenData/FaceGeom/{plugin}/{formId}.nif
        public string? FacegenNifPath    { get; init; }
        public string? PluginSource      { get; init; }
    }

    private record NpcCreateRequest
    {
        public string? EditorId    { get; init; }
        public string? Name        { get; init; }
        // Mutagen FormKey string: "RRRRRR:PluginName.esm"
        public string? RaceFormKey { get; init; }
        public bool    IsFemale    { get; init; }
    }
}
