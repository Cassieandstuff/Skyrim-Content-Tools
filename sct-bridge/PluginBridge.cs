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
///   s_loadedMods    — all mods in current session (masters first, source/overrides last)
///   s_linkCache     — covers all s_loadedMods for full record resolution
///   s_projectMod    — writable mod; accumulates records created from SCT UI
///
/// Load modes:
///   sct_plugin_load      — single plugin + its declared direct masters
///   sct_load_order_load  — full load order from plugins.txt (or all ESM/ESP/ESL in data folder)
/// </summary>
public static class PluginBridge
{
    [ThreadStatic]
    private static string? s_lastError;

    private static List<ISkyrimModGetter> s_loadedMods = new();
    private static ILinkCache?            s_linkCache;
    private static SkyrimMod?             s_projectMod;
    private static string                 s_dataFolder = "";

    private static readonly JsonSerializerOptions s_json = new()
    {
        PropertyNamingPolicy   = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    // ── Source plugin ─────────────────────────────────────────────────────────
    //
    // Loads a plugin as a read-only binary overlay, then loads all masters it
    // declares so that both link resolution AND NPC search cover the full set.
    //
    // pluginPath   — absolute path to the .esp / .esm / .esl
    // dataFolder   — game Data directory (used to locate master files)
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_plugin_load")]
    public static unsafe int PluginLoad(byte* pluginPathUtf8, byte* dataFolderUtf8)
    {
        try
        {
            var pluginPath = Marshal.PtrToStringUTF8((IntPtr)pluginPathUtf8)
                             ?? throw new ArgumentException("null plugin path");
            s_dataFolder   = Marshal.PtrToStringUTF8((IntPtr)dataFolderUtf8) ?? "";

            var source = SkyrimMod.CreateFromBinaryOverlay(
                new ModPath(pluginPath), SkyrimRelease.SkyrimSE);

            // Load declared masters (direct only — covers the common Skyrim.esm /
            // Update.esm case without risk of deep recursive chains).
            var mods = new List<ISkyrimModGetter>();
            foreach (var master in source.MasterReferences)
            {
                var masterPath = Path.Combine(s_dataFolder, master.Master.FileName);
                if (!File.Exists(masterPath)) continue;
                try
                {
                    mods.Add(SkyrimMod.CreateFromBinaryOverlay(
                        new ModPath(masterPath), SkyrimRelease.SkyrimSE));
                }
                catch { /* skip unloadable masters */ }
            }
            mods.Add(source); // highest priority last

            s_loadedMods = mods;
            s_linkCache  = s_loadedMods.ToUntypedImmutableLinkCache();

            s_lastError = null;
            return 0;
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── Load Order ────────────────────────────────────────────────────────────
    //
    // Loads the full active load order into s_loadedMods.
    // Reads plugins.txt from the standard SE AppData location; falls back to
    // all ESM/ESL files in the data folder if plugins.txt is not found.
    // After this call, NpcSearch covers every loaded plugin.
    //
    // dataFolder — game Data directory
    // Returns the number of mods loaded on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_load_order_load")]
    public static unsafe int LoadOrderLoad(byte* dataFolderUtf8)
    {
        try
        {
            s_dataFolder = Marshal.PtrToStringUTF8((IntPtr)dataFolderUtf8) ?? "";

            var pluginNames = ResolveLoadOrder(s_dataFolder);

            var mods = new List<ISkyrimModGetter>();
            foreach (var name in pluginNames)
            {
                var path = Path.Combine(s_dataFolder, name);
                if (!File.Exists(path)) continue;
                try
                {
                    mods.Add(SkyrimMod.CreateFromBinaryOverlay(
                        new ModPath(path), SkyrimRelease.SkyrimSE));
                }
                catch { /* skip plugins that fail to load */ }
            }

            s_loadedMods = mods;
            s_linkCache  = s_loadedMods.ToUntypedImmutableLinkCache();
            s_lastError  = null;
            return mods.Count;
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    /// <summary>
    /// Returns plugin filenames in load order.  Reads plugins.txt (checking
    /// standard SSE, GOG, and Xbox Game Pass AppData paths).  Falls back to
    /// all ESM+ESL files alphabetically if no plugins.txt is found.
    /// </summary>
    private static IEnumerable<string> ResolveLoadOrder(string dataFolder)
    {
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string[] candidates =
        [
            Path.Combine(appData, "Skyrim Special Edition",      "plugins.txt"),
            Path.Combine(appData, "Skyrim Special Edition GOG",  "plugins.txt"),
            Path.Combine(appData, "Skyrim Special Edition MS",   "plugins.txt"),
        ];

        var pluginsFile = candidates.FirstOrDefault(File.Exists);
        if (pluginsFile is not null)
        {
            // Each enabled line is prefixed with '*'.  Disabled lines have no prefix.
            return File.ReadAllLines(pluginsFile)
                .Where(l => l.StartsWith('*'))
                .Select(l => l[1..].Trim())
                .Where(l => l.Length > 0);
        }

        // Fallback: load all ESM/ESL from the data folder (omit loose ESPs —
        // they have no guaranteed load order without a plugins.txt).
        return Directory
            .EnumerateFiles(dataFolder, "*.*", SearchOption.TopDirectoryOnly)
            .Where(f => { var e = Path.GetExtension(f).ToLower(); return e == ".esm" || e == ".esl"; })
            .Select(Path.GetFileName)
            .OfType<string>()
            .OrderBy(n => n, StringComparer.OrdinalIgnoreCase);
    }

    [UnmanagedCallersOnly(EntryPoint = "sct_plugin_unload")]
    public static void PluginUnload()
    {
        s_loadedMods.Clear();
        s_linkCache = null;
    }

    // ── NPC search ────────────────────────────────────────────────────────────
    //
    // Searches ALL loaded mods (masters + source plugin) for NPC_ records whose
    // name or EditorID contains the query string (case-insensitive).
    // Empty query returns up to maxResults records from the combined set.
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
            if (s_loadedMods.Count == 0)
                throw new InvalidOperationException("no plugin loaded — call sct_plugin_load first");

            var query   = Marshal.PtrToStringUTF8((IntPtr)queryUtf8) ?? "";
            var cap     = maxResults > 0 ? maxResults : 100;

            // Search in reverse priority order so higher-priority records appear first.
            var records = s_loadedMods
                .AsEnumerable()
                .Reverse()
                .SelectMany(m => m.Npcs)
                .Where(n => MatchesQuery(n, query))
                .Take(cap)
                .Select(ToNpcRecord)
                .ToList();

            return WriteJson(records, outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── NPC get by FormID ─────────────────────────────────────────────────────

    [UnmanagedCallersOnly(EntryPoint = "sct_npc_get")]
    public static unsafe int NpcGet(uint formId, byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_loadedMods.Count == 0)
                throw new InvalidOperationException("no plugin loaded");

            // Search all loaded mods; return highest-priority (last) match.
            var npc = s_loadedMods
                .SelectMany(m => m.Npcs)
                .LastOrDefault(n => n.FormKey.ID == formId)
                ?? throw new KeyNotFoundException($"FormID {formId:X8} not found");

            return WriteJson(ToNpcRecord(npc), outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── Project mod ───────────────────────────────────────────────────────────

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
            // C++ converts this to an HKX path via a .nif → .hkx extension swap
            // and a "meshes\" prefix, then auto-matches against discoveredSkeletons.
            if (npc.Race.TryResolve(s_linkCache, out var race))
            {
                raceEditorId      = race.EditorID;
                var isFemale      = npc.Configuration.Flags.HasFlag(NpcConfiguration.Flag.Female);
                var skelModel     = isFemale ? race.SkeletalModel?.Female : race.SkeletalModel?.Male;
                skeletonModelPath = skelModel?.File.GivenPath;
            }

            // Head parts → expression TRI path.
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
        // Skeleton NIF path from the race record (relative to Data\Meshes\, no prefix).
        // e.g. "Actors\Character\Character Assets\Skeleton.nif"
        public string? SkeletonModelPath { get; init; }
        public string? ExpressionTriPath { get; init; }
        // Derived: Meshes/Actors/Character/FaceGenData/FaceGeom/{plugin}/{formId}.nif
        public string? FacegenNifPath    { get; init; }
        public string? PluginSource      { get; init; }
    }

    private record NpcCreateRequest
    {
        public string? EditorId    { get; init; }
        public string? Name        { get; init; }
        public string? RaceFormKey { get; init; }
        public bool    IsFemale    { get; init; }
    }
}
