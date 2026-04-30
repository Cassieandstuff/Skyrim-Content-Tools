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

    // ── Cell search ──────────────────────────────────────────────────────────
    //
    // Searches interior cells across all loaded mods.  Uses mod.Cells (the
    // top-level GRUP CELL block), which in Skyrim's binary format contains
    // only interior cells; exterior cells live under GRUP WRLD and are
    // not included here.
    //
    // query      — substring matched against EditorID and display name (empty = all)
    // maxResults — cap on returned records; 0 → 200
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_cell_search")]
    public static unsafe int CellSearch(byte* queryUtf8, int maxResults,
                                         byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_loadedMods.Count == 0)
                throw new InvalidOperationException("no plugin loaded — call sct_plugin_load first");

            var query = Marshal.PtrToStringUTF8((IntPtr)queryUtf8) ?? "";
            var cap   = maxResults > 0 ? maxResults : 200;

            // Walk block → subblock → cell hierarchy for each mod (highest-priority first).
            var records = s_loadedMods
                .AsEnumerable()
                .Reverse()
                .SelectMany(m => m.Cells.Records
                    .SelectMany(b => b.SubBlocks)
                    .SelectMany(sb => sb.Cells))
                .Where(c => MatchesCellQuery(c, query))
                .Take(cap)
                .Select(ToCellRecord)
                .ToList();

            return WriteJson(records, outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── Cell get refs ─────────────────────────────────────────────────────────
    //
    // Returns all placed objects in an interior cell — the Temporary and
    // Persistent reference lists.  Each entry includes the base object's NIF
    // model path (resolved through the link cache) and its placement transform.
    // Records whose base object cannot be resolved to a model are silently
    // skipped (e.g. lights, markers, trigger volumes).
    //
    // The baseFormKey field is stable across identical placements of the same
    // base object; callers should use it as the instancing key when building
    // a mesh catalog (one GPU upload per unique baseFormKey).
    //
    // formKeyStr — FormKey string as produced by sct_cell_search ("XXXXXXXX:Plugin.esm")
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_cell_get_refs")]
    public static unsafe int CellGetRefs(byte* formKeyUtf8, byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_loadedMods.Count == 0 || s_linkCache is null)
                throw new InvalidOperationException("no plugin loaded");

            var formKeyStr = Marshal.PtrToStringUTF8((IntPtr)formKeyUtf8)
                             ?? throw new ArgumentException("null formKey");

            if (!FormKey.TryFactory(formKeyStr, out var formKey))
                throw new ArgumentException($"invalid form key: '{formKeyStr}'");

            // Find cell — highest-priority mod wins (last match).
            ICellGetter? cell = null;
            foreach (var mod in s_loadedMods.AsEnumerable().Reverse())
            {
                cell = mod.Cells.Records
                    .SelectMany(b => b.SubBlocks)
                    .SelectMany(sb => sb.Cells)
                    .FirstOrDefault(c => c.FormKey == formKey);
                if (cell is not null) break;
            }

            if (cell is null)
                throw new KeyNotFoundException($"cell {formKeyStr} not found");

            // Concatenate temporary + persistent refs; only process placed objects.
            var allRefs = (cell.Temporary   ?? Enumerable.Empty<IPlacedGetter>())
                .Concat(cell.Persistent ?? Enumerable.Empty<IPlacedGetter>());

            var refs = new List<CellRefRecord>();
            foreach (var placed in allRefs)
            {
                if (placed is not IPlacedObjectGetter pObj) continue;

                var placement = pObj.Placement;
                if (placement is null) continue;

                var (nifPath, baseEditorId) = ResolveBaseModelPath(pObj.Base.FormKey);
                if (string.IsNullOrEmpty(nifPath)) continue;

                refs.Add(new CellRefRecord
                {
                    RefFormKey   = pObj.FormKey.ToString(),
                    BaseFormKey  = pObj.Base.FormKey.ToString(),
                    BaseEditorId = baseEditorId,
                    NifPath      = EnsureMeshesPrefix(nifPath),
                    PosX  = placement.Position.X,
                    PosY  = placement.Position.Y,
                    PosZ  = placement.Position.Z,
                    RotX  = placement.Rotation.X,
                    RotY  = placement.Rotation.Y,
                    RotZ  = placement.Rotation.Z,
                    Scale = pObj.Scale ?? 1.0f,
                });
            }

            return WriteJson(refs, outJson, outLen);
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

    // ── Worldspace search ─────────────────────────────────────────────────────
    //
    // Returns worldspace records (Tamriel, Solstheim, interiors-as-worlds, etc.)
    // matching the query.  Empty query returns all worldspaces up to maxResults.
    //
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_worldspace_search")]
    public static unsafe int WorldspaceSearch(byte* queryUtf8, int maxResults,
                                               byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_loadedMods.Count == 0)
                throw new InvalidOperationException("no plugin loaded — call sct_plugin_load first");

            var query = Marshal.PtrToStringUTF8((IntPtr)queryUtf8) ?? "";
            var cap   = maxResults > 0 ? maxResults : 200;

            var records = s_loadedMods
                .AsEnumerable()
                .Reverse()
                .SelectMany(m => m.Worldspaces)
                .Where(w => MatchesWorldspaceQuery(w, query))
                .Take(cap)
                .Select(ToWorldspaceRecord)
                .ToList();

            return WriteJson(records, outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── Exterior cell get refs ────────────────────────────────────────────────
    //
    // Returns placed refs for the exterior cell at grid (cellX, cellY) within
    // the given worldspace.  Same JSON format as sct_cell_get_refs.
    //
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_exterior_cell_get_refs")]
    public static unsafe int ExteriorCellGetRefs(byte* wsFormKeyUtf8, int cellX, int cellY,
                                                  byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_loadedMods.Count == 0 || s_linkCache is null)
                throw new InvalidOperationException("no plugin loaded");

            var wsFormKeyStr = Marshal.PtrToStringUTF8((IntPtr)wsFormKeyUtf8)
                               ?? throw new ArgumentException("null worldspace formKey");

            if (!FormKey.TryFactory(wsFormKeyStr, out var wsFormKey))
                throw new ArgumentException($"invalid worldspace form key: '{wsFormKeyStr}'");

            IWorldspaceGetter? ws = null;
            foreach (var mod in s_loadedMods.AsEnumerable().Reverse())
            {
                ws = mod.Worldspaces.FirstOrDefault(w => w.FormKey == wsFormKey);
                if (ws is not null) break;
            }
            if (ws is null)
                throw new KeyNotFoundException($"worldspace {wsFormKeyStr} not found");

            // Find the exterior cell at the requested grid position.
            var cell = ws.SubCells
                .SelectMany(b => b.Items)
                .SelectMany(sb => sb.Items)
                .FirstOrDefault(c => c.Grid?.Point.X == (short)cellX
                                  && c.Grid?.Point.Y == (short)cellY);
            if (cell is null)
                throw new KeyNotFoundException(
                    $"exterior cell ({cellX},{cellY}) not found in worldspace {wsFormKeyStr}");

            var allRefs = (cell.Temporary   ?? Enumerable.Empty<IPlacedGetter>())
                .Concat(cell.Persistent ?? Enumerable.Empty<IPlacedGetter>());

            var refs = new List<CellRefRecord>();
            foreach (var placed in allRefs)
            {
                if (placed is not IPlacedObjectGetter pObj) continue;
                var placement = pObj.Placement;
                if (placement is null) continue;

                var (nifPath, baseEditorId) = ResolveBaseModelPath(pObj.Base.FormKey);
                if (string.IsNullOrEmpty(nifPath)) continue;

                refs.Add(new CellRefRecord
                {
                    RefFormKey   = pObj.FormKey.ToString(),
                    BaseFormKey  = pObj.Base.FormKey.ToString(),
                    BaseEditorId = baseEditorId,
                    NifPath      = EnsureMeshesPrefix(nifPath),
                    PosX  = placement.Position.X,
                    PosY  = placement.Position.Y,
                    PosZ  = placement.Position.Z,
                    RotX  = placement.Rotation.X,
                    RotY  = placement.Rotation.Y,
                    RotZ  = placement.Rotation.Z,
                    Scale = pObj.Scale ?? 1.0f,
                });
            }

            return WriteJson(refs, outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── Land get data ─────────────────────────────────────────────────────────
    //
    // Decodes the LAND (landscape) record for the exterior cell at (cellX, cellY)
    // in the given worldspace and returns terrain heights + vertex colours.
    //
    // JSON response: { "heights": [float x 1089], "colors": [byte x 3267 or empty] }
    // Heights are world-space Z values in Skyrim units (row-major, 33×33 grid).
    // Colors are flat RGB bytes (r0,g0,b0, r1,g1,b1, ...) or an empty array if
    // the LAND record has no VCLR sub-record.
    //
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_land_get_data")]
    public static unsafe int LandGetData(byte* wsFormKeyUtf8, int cellX, int cellY,
                                          byte** outJson, int* outLen)
    {
        *outJson = null;
        *outLen  = 0;
        try
        {
            if (s_loadedMods.Count == 0 || s_linkCache is null)
                throw new InvalidOperationException("no plugin loaded");

            var wsFormKeyStr = Marshal.PtrToStringUTF8((IntPtr)wsFormKeyUtf8)
                               ?? throw new ArgumentException("null worldspace formKey");

            if (!FormKey.TryFactory(wsFormKeyStr, out var wsFormKey))
                throw new ArgumentException($"invalid worldspace form key: '{wsFormKeyStr}'");

            IWorldspaceGetter? ws = null;
            foreach (var mod in s_loadedMods.AsEnumerable().Reverse())
            {
                ws = mod.Worldspaces.FirstOrDefault(w => w.FormKey == wsFormKey);
                if (ws is not null) break;
            }
            if (ws is null)
                throw new KeyNotFoundException($"worldspace {wsFormKeyStr} not found");

            var cell = ws.SubCells
                .SelectMany(b => b.Items)
                .SelectMany(sb => sb.Items)
                .FirstOrDefault(c => c.Grid?.Point.X == (short)cellX
                                  && c.Grid?.Point.Y == (short)cellY);
            if (cell is null)
                throw new KeyNotFoundException(
                    $"exterior cell ({cellX},{cellY}) not found");

            var landscape = cell.Landscape;
            if (landscape is null)
                throw new InvalidOperationException(
                    $"cell ({cellX},{cellY}) has no LAND record");

            // ── VHGT decode ───────────────────────────────────────────────────
            // Column 0 accumulates down the grid (each row's col-0 is a delta from
            // the previous row's col-0).  Each row then accumulates right from its
            // col-0 value.  Final world height = accumulated_value × 8.
            var vhgt    = landscape.VertexHeightMap;
            var heights = new float[33 * 33];
            if (vhgt is not null)
            {
                // HeightMap is IReadOnlyArray2d<byte> indexed by P2Int(col, row).
                // Values are signed height deltas — cast each byte to sbyte.
                var raw    = vhgt.HeightMap;
                float col0 = vhgt.Offset;
                for (int r = 0; r < 33; r++)
                {
                    col0 += (sbyte)raw[new Noggog.P2Int(0, r)];
                    float h = col0;
                    heights[r * 33 + 0] = h * 8.0f;
                    for (int c = 1; c < 33; c++)
                    {
                        h += (sbyte)raw[new Noggog.P2Int(c, r)];
                        heights[r * 33 + c] = h * 8.0f;
                    }
                }
            }

            // ── VCLR decode ───────────────────────────────────────────────────
            // VertexColors is IReadOnlyArray2d<P3UInt8> indexed by P2Int(col, row).
            // P3UInt8.X=R, .Y=G, .Z=B.  Flatten to row-major int[33*33*3] (values 0-255).
            // int[] not byte[]: System.Text.Json encodes byte[] as Base64 which C++ can't parse.
            var colors = Array.Empty<int>();
            if (landscape.VertexColors is { } vclrData)
            {
                colors = new int[33 * 33 * 3];
                for (int r = 0; r < 33; r++)
                for (int c = 0; c < 33; c++)
                {
                    var px = vclrData[new Noggog.P2Int(c, r)];
                    int i  = (r * 33 + c) * 3;
                    colors[i]     = px.X;
                    colors[i + 1] = px.Y;
                    colors[i + 2] = px.Z;
                }
            }

            return WriteJson(new LandData { Heights = heights, Colors = colors },
                             outJson, outLen);
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    // ── Worldspace get terrain bulk ───────────────────────────────────────────
    //
    // Returns all LAND records for a worldspace as a compact binary blob (SLRT v2).
    // Header: magic(4) "SLRT" + version(4) 2 + cellCount(4).
    // Per cell: int16 cellX + int16 cellY
    //   + float[1089] heights
    //   + uint8 hasColors + [if hasColors] uint8[3267] colors
    //   + uint16 basePathLen + [if >0] char[basePathLen] + float baseTileRate
    //   + uint8 alphaLayerCount (0-5)
    //   + for each alpha layer:
    //       uint16 pathLen + char[pathLen] path + float tileRate
    //       + float[1089] blendMap (33×33, row-major, 0.0-1.0)
    // All values little-endian.  Caller frees via sct_free_buffer.
    //
    // Returns 0 on success, -1 on failure.

    [UnmanagedCallersOnly(EntryPoint = "sct_worldspace_get_terrain_bulk")]
    public static unsafe int WorldspaceGetTerrainBulk(byte* wsFormKeyUtf8,
                                                       byte** outData, int* outLen)
    {
        *outData = null;
        *outLen  = 0;
        try
        {
            if (s_loadedMods.Count == 0 || s_linkCache is null)
                throw new InvalidOperationException("no plugin loaded");

            var wsFormKeyStr = Marshal.PtrToStringUTF8((IntPtr)wsFormKeyUtf8)
                               ?? throw new ArgumentException("null worldspace formKey");

            if (!FormKey.TryFactory(wsFormKeyStr, out var wsFormKey))
                throw new ArgumentException($"invalid worldspace form key: '{wsFormKeyStr}'");

            IWorldspaceGetter? ws = null;
            foreach (var mod in s_loadedMods.AsEnumerable().Reverse())
            {
                ws = mod.Worldspaces.FirstOrDefault(w => w.FormKey == wsFormKey);
                if (ws is not null) break;
            }
            if (ws is null)
                throw new KeyNotFoundException($"worldspace {wsFormKeyStr} not found");

            // Collect all exterior cells that have a LAND record.
            var cells = ws.SubCells
                .SelectMany(b => b.Items)
                .SelectMany(sb => sb.Items)
                .Where(c => c.Landscape is not null && c.Grid is not null)
                .ToList();

            using var ms = new System.IO.MemoryStream();
            using var w  = new System.IO.BinaryWriter(ms, Encoding.UTF8, leaveOpen: true);

            // Header
            w.Write((uint)0x54524C53u); // "SLRT"
            w.Write((uint)2u);          // version 2: includes alpha layers
            w.Write((uint)cells.Count);

            foreach (var cell in cells)
            {
                w.Write((short)cell.Grid!.Point.X);
                w.Write((short)cell.Grid!.Point.Y);
                WriteCellTerrain(w, cell.Landscape!);
            }

            w.Flush();
            var bytes = ms.ToArray();
            var ptr   = (byte*)Marshal.AllocCoTaskMem(bytes.Length);
            bytes.AsSpan().CopyTo(new Span<byte>(ptr, bytes.Length));
            *outData    = ptr;
            *outLen     = bytes.Length;
            s_lastError = null;
            return 0;
        }
        catch (Exception ex) { s_lastError = ex.Message; return -1; }
    }

    /// <summary>
    /// Writes the VHGT/VCLR/BTXT terrain data for one LAND record into a BinaryWriter.
    /// Format: float[1089] heights + uint8 hasColors + [if hasColors] uint8[3267] colors
    ///         + uint16 pathLen + [if pathLen>0] char[pathLen] texPath + float tileRate.
    /// </summary>
    private static void WriteCellTerrain(System.IO.BinaryWriter w, ILandscapeGetter landscape)
    {
        // ── VHGT decode ───────────────────────────────────────────────────────
        var vhgt    = landscape.VertexHeightMap;
        var heights = new float[33 * 33];
        if (vhgt is not null)
        {
            var raw    = vhgt.HeightMap;
            float col0 = vhgt.Offset;
            for (int r = 0; r < 33; r++)
            {
                col0 += (sbyte)raw[new Noggog.P2Int(0, r)];
                float h = col0;
                heights[r * 33 + 0] = h * 8.0f;
                for (int c = 1; c < 33; c++)
                {
                    h += (sbyte)raw[new Noggog.P2Int(c, r)];
                    heights[r * 33 + c] = h * 8.0f;
                }
            }
        }
        foreach (var f in heights) w.Write(f);

        // ── VCLR ─────────────────────────────────────────────────────────────
        if (landscape.VertexColors is { } vclrData)
        {
            w.Write((byte)1); // hasColors = true
            for (int r = 0; r < 33; r++)
            for (int c = 0; c < 33; c++)
            {
                var px = vclrData[new Noggog.P2Int(c, r)];
                w.Write(px.X);
                w.Write(px.Y);
                w.Write(px.Z);
            }
        }
        else
        {
            w.Write((byte)0); // hasColors = false
        }

        // ── Base texture path (BTXT layer 0 → LTEX → TXST → Diffuse) ─────────
        // BTXT records are base layers (not alpha overlays); filter by type.
        string? baseDiffusePath = null;
        try
        {
            if (landscape.Layers is { } layers && s_linkCache is not null)
            {
                var baseLayer = layers.FirstOrDefault(l => l is not IAlphaLayerGetter);
                if (baseLayer is not null &&
                    baseLayer.Header.Texture.TryResolve<ILandscapeTextureGetter>(s_linkCache, out var ltex) &&
                    ltex.TextureSet.TryResolve<ITextureSetGetter>(s_linkCache, out var txst))
                {
                    baseDiffusePath = txst.Diffuse?.GivenPath;
                }
            }
        }
        catch { /* broken record — skip texture */ }

        if (!string.IsNullOrEmpty(baseDiffusePath))
        {
            if (!baseDiffusePath.StartsWith("Textures\\", StringComparison.OrdinalIgnoreCase) &&
                !baseDiffusePath.StartsWith("Textures/",  StringComparison.OrdinalIgnoreCase))
                baseDiffusePath = "Textures\\" + baseDiffusePath;
            var pathBytes = Encoding.UTF8.GetBytes(baseDiffusePath);
            w.Write((ushort)pathBytes.Length);
            w.Write(pathBytes);
            w.Write(6.0f); // UV tile rate — Skyrim terrain default
        }
        else
        {
            w.Write((ushort)0);
        }

        // ── Alpha layers (ATXT + VTXT) ────────────────────────────────────────
        // Collect unique alpha textures across all 4 quadrants.  For each unique
        // texture, stitch the sparse per-quadrant VTXT data into a dense 33×33
        // float blend map (same vertex grid as VHGT/VCLR).
        //
        // Quadrant offsets in the 33×33 grid (row increases northward in Skyrim):
        //   BottomLeft(0)  → rowOff=0,  colOff=0
        //   BottomRight(1) → rowOff=0,  colOff=16
        //   UpperLeft(2)   → rowOff=16, colOff=0
        //   UpperRight(3)  → rowOff=16, colOff=16
        var alphaTexPaths  = new List<string>();
        var alphaBlendMaps = new List<float[,]>();

        try
        {
            if (landscape.Layers is { } allLayers && s_linkCache is not null)
            {
                foreach (var layer in allLayers.OfType<IAlphaLayerGetter>())
                {
                    // Resolve ATXT texture path
                    string? aPath = null;
                    try
                    {
                        if (layer.Header.Texture.TryResolve<ILandscapeTextureGetter>(s_linkCache, out var ltex) &&
                            ltex.TextureSet.TryResolve<ITextureSetGetter>(s_linkCache, out var txst))
                        {
                            aPath = txst.Diffuse?.GivenPath;
                            if (!string.IsNullOrEmpty(aPath) &&
                                !aPath.StartsWith("Textures\\", StringComparison.OrdinalIgnoreCase) &&
                                !aPath.StartsWith("Textures/",  StringComparison.OrdinalIgnoreCase))
                                aPath = "Textures\\" + aPath;
                        }
                    }
                    catch { }
                    if (string.IsNullOrEmpty(aPath)) continue;

                    // Find or create blend-map slot for this texture
                    int idx = alphaTexPaths.IndexOf(aPath);
                    if (idx < 0)
                    {
                        if (alphaTexPaths.Count >= 5) continue; // cap at 5 alpha layers
                        idx = alphaTexPaths.Count;
                        alphaTexPaths.Add(aPath);
                        alphaBlendMaps.Add(new float[33, 33]);
                    }

                    // Quadrant → 33×33 offset
                    var q = (byte)layer.Header.Quadrant;
                    int rowOff = (q == 2 || q == 3) ? 16 : 0;
                    int colOff = (q == 1 || q == 3) ? 16 : 0;

                    // Decode sparse VTXT → dense quadrant 17×17, write into blend map
                    try
                    {
                        if (layer.AlphaLayerData is { } entries)
                        {
                            var bm = alphaBlendMaps[idx];
                            foreach (var entry in entries)
                            {
                                int pos = entry.Position;
                                int r   = pos / 17;
                                int c   = pos % 17;
                                if (r > 16 || c > 16) continue;
                                // Opacity stored as UInt16; 0x8000 (32768) = fully opaque.
                                float opacity = (float)entry.Opacity / 32768.0f;
                                bm[rowOff + r, colOff + c] = Math.Clamp(opacity, 0f, 1f);
                            }
                        }
                    }
                    catch { }
                }
            }
        }
        catch { }

        // Write alpha layers
        int layerCount = Math.Min(alphaTexPaths.Count, 5);
        w.Write((byte)layerCount);
        for (int li = 0; li < layerCount; li++)
        {
            var pb = Encoding.UTF8.GetBytes(alphaTexPaths[li]);
            w.Write((ushort)pb.Length);
            w.Write(pb);
            w.Write(6.0f); // tileRate — Skyrim terrain default
            var bm = alphaBlendMaps[li];
            for (int r = 0; r < 33; r++)
            for (int c = 0; c < 33; c++)
                w.Write(bm[r, c]);
        }
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

    private static bool MatchesCellQuery(ICellGetter cell, string query)
    {
        if (string.IsNullOrWhiteSpace(query)) return true;
        var q = query.Trim();
        return cell.EditorID?.Contains(q, StringComparison.OrdinalIgnoreCase) == true
            || cell.Name?.String?.Contains(q, StringComparison.OrdinalIgnoreCase) == true;
    }

    private static CellRecord ToCellRecord(ICellGetter cell) => new()
    {
        FormId      = cell.FormKey.ID,
        FormKey     = cell.FormKey.ToString(),
        EditorId    = cell.EditorID,
        Name        = cell.Name?.String,
        PluginSource = cell.FormKey.ModKey.FileName,
    };

    /// <summary>
    /// Resolves a placed-object base FormKey to its NIF model path and EditorID.
    /// Tries the most common static types in frequency order; returns (null, null)
    /// for record types that carry no geometry (lights, markers, etc.).
    /// </summary>
    private static (string? nifPath, string? editorId) ResolveBaseModelPath(FormKey baseFormKey)
    {
        if (s_linkCache is null) return (null, null);

        if (s_linkCache.TryResolve<IStaticGetter>(baseFormKey, out var stat))
            return (stat.Model?.File.GivenPath, stat.EditorID);
        if (s_linkCache.TryResolve<IMoveableStaticGetter>(baseFormKey, out var mstat))
            return (mstat.Model?.File.GivenPath, mstat.EditorID);
        if (s_linkCache.TryResolve<IFurnitureGetter>(baseFormKey, out var furn))
            return (furn.Model?.File.GivenPath, furn.EditorID);
        if (s_linkCache.TryResolve<IContainerGetter>(baseFormKey, out var cont))
            return (cont.Model?.File.GivenPath, cont.EditorID);
        if (s_linkCache.TryResolve<IDoorGetter>(baseFormKey, out var door))
            return (door.Model?.File.GivenPath, door.EditorID);
        if (s_linkCache.TryResolve<ITreeGetter>(baseFormKey, out var tree))
            return (tree.Model?.File.GivenPath, tree.EditorID);
        if (s_linkCache.TryResolve<IFloraGetter>(baseFormKey, out var flora))
            return (flora.Model?.File.GivenPath, flora.EditorID);
        if (s_linkCache.TryResolve<IActivatorGetter>(baseFormKey, out var acti))
            return (acti.Model?.File.GivenPath, acti.EditorID);

        return (null, null);
    }

    private static bool MatchesQuery(INpcGetter npc, string query)
    {
        if (string.IsNullOrWhiteSpace(query)) return true;
        var q = query.Trim();
        return npc.Name?.String?.Contains(q, StringComparison.OrdinalIgnoreCase) == true
            || npc.EditorID?.Contains(q,    StringComparison.OrdinalIgnoreCase) == true;
    }

    private static NpcRecord ToNpcRecord(INpcGetter npc)
    {
        string? skeletonModelPath     = null;
        string? raceEditorId          = null;
        IRaceGetter? race             = null;
        var expressionTriPaths        = new List<string>();

        var isFemale = npc.Configuration.Flags.HasFlag(NpcConfiguration.Flag.Female);
        var bodyParts    = new List<BodyPartEntry>();
        var headPartNifs = new List<string>();

        if (s_linkCache is not null)
        {
            // Race → skeleton NIF path.
            if (npc.Race.TryResolve<IRaceGetter>(s_linkCache, out race))
            {
                raceEditorId      = race.EditorID;
                var skelModel     = isFemale ? race.SkeletalModel?.Female : race.SkeletalModel?.Male;
                skeletonModelPath = skelModel?.File.GivenPath;
            }

            // ── Body parts: WornArmor → Armor → ArmorAddon ────────────────────
            // NPC_.WornArmor (WNAM) is the equipped skin armor (e.g. SkinNaked).
            // Its Armature list contains ArmorAddons for each body slot.
            // Fallback: when the NPC has no WornArmor (e.g. the Player), use
            // the Race's default skin armor instead.
            try
            {
                IArmorGetter? armor = null;

                // Primary: NPC's own WornArmor
                try
                {
                    npc.WornArmor.TryResolve<IArmorGetter>(s_linkCache, out armor);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine(
                        $"[SCT] WornArmor access failed for {npc.FormKey}: {ex.Message}");
                }

                // Fallback: Race's default skin armor (covers Player and bare-NPC records)
                if (armor is null && race is not null)
                {
                    try { race.Skin.TryResolve<IArmorGetter>(s_linkCache, out armor); }
                    catch { /* race has no skin — skip */ }
                }

                if (armor is not null)
                {
                    foreach (var aaLink in armor.Armature)
                    {
                        if (!aaLink.TryResolve<IArmorAddonGetter>(s_linkCache, out var aa)) continue;

                        var flags = aa.BodyTemplate?.FirstPersonFlags
                                    ?? default(BipedObjectFlag);

                        // Only care about the three naked-body slots.
                        string? slot = null;
                        if      (flags.HasFlag(BipedObjectFlag.Body))  slot = "body";
                        else if (flags.HasFlag(BipedObjectFlag.Hands)) slot = "hands";
                        else if (flags.HasFlag(BipedObjectFlag.Feet))  slot = "feet";
                        if (slot is null) continue;

                        var model   = isFemale ? aa.WorldModel?.Female : aa.WorldModel?.Male;
                        var rawPath = model?.File.GivenPath;
                        if (string.IsNullOrEmpty(rawPath)) continue;

                        // Ensure the path is relative to Data root with Meshes\ prefix.
                        var nifPath = EnsureMeshesPrefix(rawPath);
                        bodyParts.Add(new BodyPartEntry { Slot = slot, NifPath = nifPath });
                    }
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"[SCT] Body part resolve failed for {npc.FormKey}: {ex.Message}");
            }

            // ── Head parts ─────────────────────────────────────────────────────
            // 1. Race-default head parts (ears, brows, teeth, mouth, eyes…).
            //    These carry the primary face expression TRIs (malehead.tri,
            //    mouthhuman.tri, maleheadbrows.tri, etc.) that MFEE extends.
            try
            {
                var raceHeadData = isFemale ? race?.HeadData?.Female : race?.HeadData?.Male;
                if (raceHeadData?.HeadParts is { } raceHPs)
                {
                    foreach (var hpRef in raceHPs)
                    {
                        if (!hpRef.Head.TryResolve<IHeadPartGetter>(s_linkCache, out var hp)) continue;

                        // Head part NIF (for rendering)
                        var rawPath = hp.Model?.File.GivenPath;
                        if (!string.IsNullOrEmpty(rawPath))
                            headPartNifs.Add(EnsureMeshesPrefix(rawPath));

                        // Convention-based TRI: face expression head parts (MaleHead,
                        // MouthHuman, MaleHeadBrows, etc.) do NOT store their TRI path
                        // in hp.Parts — the TRI is co-located with the NIF.
                        var derivedTri = DeriveExpressionTriPath(rawPath);
                        if (!string.IsNullOrEmpty(derivedTri) &&
                            !expressionTriPaths.Contains(derivedTri, StringComparer.OrdinalIgnoreCase))
                            expressionTriPaths.Add(derivedTri);

                        // Explicit TRI sub-records (belt-and-suspenders — catches hair
                        // morphs and any other parts that do store TRI paths explicitly).
                        foreach (var part in hp.Parts)
                        {
                            if (part.PartType != Part.PartTypeEnum.Tri) continue;
                            var triPath = part.FileName?.GivenPath;
                            if (!string.IsNullOrEmpty(triPath) &&
                                !expressionTriPaths.Contains(triPath, StringComparer.OrdinalIgnoreCase))
                                expressionTriPaths.Add(triPath);
                        }
                    }
                }
            }
            catch { /* partial failure — skip race head parts */ }

            // 2. NPC-chosen head parts (hair, eyes, custom face parts).
            //    Collect ALL expression TRI paths across all head parts.
            foreach (var hpLink in npc.HeadParts)
            {
                if (!hpLink.TryResolve<IHeadPartGetter>(s_linkCache, out var hp)) continue;

                // Collect every TRI path in this head part's Parts list.
                foreach (var part in hp.Parts)
                {
                    if (part.PartType != Part.PartTypeEnum.Tri) continue;
                    var triPath = part.FileName?.GivenPath;
                    if (!string.IsNullOrEmpty(triPath) &&
                        !expressionTriPaths.Contains(triPath, StringComparer.OrdinalIgnoreCase))
                        expressionTriPaths.Add(triPath);
                }

                // Head part NIF (the visible mesh).
                var rawPath = hp.Model?.File.GivenPath;
                if (!string.IsNullOrEmpty(rawPath))
                {
                    var nifPath = EnsureMeshesPrefix(rawPath);
                    if (!headPartNifs.Contains(nifPath, StringComparer.OrdinalIgnoreCase))
                        headPartNifs.Add(nifPath);

                    // Convention-based TRI (catches NPC-override face parts).
                    var derivedTri = DeriveExpressionTriPath(rawPath);
                    if (!string.IsNullOrEmpty(derivedTri) &&
                        !expressionTriPaths.Contains(derivedTri, StringComparer.OrdinalIgnoreCase))
                        expressionTriPaths.Add(derivedTri);
                }
            }
        }

        var pluginSource   = npc.FormKey.ModKey.FileName;
        var facegenNifPath = $"Meshes/Actors/Character/FaceGenData/FaceGeom/" +
                             $"{pluginSource}/{npc.FormKey.ID:X8}.nif";

        return new NpcRecord
        {
            FormId              = npc.FormKey.ID,
            FormKey             = npc.FormKey.ToString(),
            EditorId            = npc.EditorID,
            Name                = npc.Name?.String,
            RaceEditorId        = raceEditorId,
            IsFemale            = isFemale,
            SkeletonModelPath   = skeletonModelPath,
            ExpressionTriPaths  = [.. expressionTriPaths],
            FacegenNifPath      = facegenNifPath,
            PluginSource        = pluginSource,
            BodyParts           = [.. bodyParts],
            HeadPartNifs        = [.. headPartNifs],
        };
    }

    /// <summary>
    /// Ensures a NIF path stored in a plugin record is relative to the Data
    /// root and begins with "Meshes\".  Plugin records sometimes omit the
    /// "Meshes\" prefix (e.g. "Actors\Character\...") so we add it when absent.
    /// </summary>
    private static string EnsureMeshesPrefix(string path)
    {
        if (path.StartsWith("Meshes\\", StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith("Meshes/",  StringComparison.OrdinalIgnoreCase))
            return path;
        return "Meshes\\" + path;
    }

    /// <summary>
    /// Derives an expression TRI path from a head part NIF model path.
    /// Face expression head parts (MaleHead, MouthHuman, MaleHeadBrows, etc.)
    /// do NOT store their TRI paths in HDPT Part sub-records — the TRI is
    /// co-located with the NIF, differing only in extension.
    ///
    /// Returns the TRI path WITHOUT the "Meshes\" prefix (matching the MFEE
    /// ini key convention), or null if the path has no extension.
    /// </summary>
    private static string? DeriveExpressionTriPath(string? nifPath)
    {
        if (string.IsNullOrEmpty(nifPath)) return null;

        // Strip Meshes\ prefix first so the result matches MFEE ini keys.
        var rel = nifPath;
        if (rel.StartsWith("Meshes\\", StringComparison.OrdinalIgnoreCase) ||
            rel.StartsWith("Meshes/",  StringComparison.OrdinalIgnoreCase))
            rel = rel.Substring(7);

        var dot = rel.LastIndexOf('.');
        if (dot < 0) return null;

        return rel.Substring(0, dot) + ".tri";
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

    private record CellRecord
    {
        public uint    FormId       { get; init; }
        public string? FormKey      { get; init; }   // "XXXXXXXX:Plugin.esm"
        public string? EditorId     { get; init; }
        public string? Name         { get; init; }   // in-game display name (FULL)
        public string? PluginSource { get; init; }
    }

    /// <summary>
    /// One placed reference from a cell's Temporary or Persistent ref list.
    /// Position and rotation are in Skyrim Z-up space (identical to Havok space):
    /// X = east, Y = north, Z = up.
    /// Rotation components (RotX, RotY, RotZ) are Euler angles in radians.
    /// Application order: extrinsic ZYX — yaw (Z) applied first in world space,
    /// then pitch (Y), then roll (X).  Matrix form: R = Rx * Ry * Rz.
    /// baseFormKey is the instancing key — identical values share the same base mesh.
    /// </summary>
    private record CellRefRecord
    {
        public string? RefFormKey   { get; init; }   // this REFR's FormKey
        public string? BaseFormKey  { get; init; }   // base object FormKey (instancing key)
        public string? BaseEditorId { get; init; }
        public string? NifPath      { get; init; }   // Data-relative, Meshes\ prefix
        public float   PosX         { get; init; }
        public float   PosY         { get; init; }
        public float   PosZ         { get; init; }
        public float   RotX         { get; init; }   // radians
        public float   RotY         { get; init; }
        public float   RotZ         { get; init; }
        public float   Scale        { get; init; } = 1.0f;
    }

    private record BodyPartEntry
    {
        public string Slot    { get; init; } = "";   // "body" | "hands" | "feet"
        public string NifPath { get; init; } = "";   // Data-relative, Meshes\ prefix
    }

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
        public string? SkeletonModelPath    { get; init; }
        // All expression TRI paths collected from all head parts (across hp.Parts where PartType==Tri).
        public string[] ExpressionTriPaths { get; init; } = [];
        // Derived: Meshes/Actors/Character/FaceGenData/FaceGeom/{plugin}/{formId}.nif
        public string? FacegenNifPath      { get; init; }
        public string? PluginSource      { get; init; }
        // Body geometry from WornArmor → Armor → ArmorAddon
        public BodyPartEntry[] BodyParts    { get; init; } = [];
        // Race-default + NPC-chosen head part NIFs (hair, eyes, mouth, ears…)
        public string[]        HeadPartNifs { get; init; } = [];
    }

    private record NpcCreateRequest
    {
        public string? EditorId    { get; init; }
        public string? Name        { get; init; }
        public string? RaceFormKey { get; init; }
        public bool    IsFemale    { get; init; }
    }

    private record WorldspaceRecord
    {
        public string? FormKey      { get; init; }
        public string? EditorId     { get; init; }
        public string? Name         { get; init; }
        public string? PluginSource { get; init; }
    }

    /// <summary>
    /// Decoded terrain data for one exterior cell.
    /// Heights: 33×33 world-space Z values (row-major, 1089 entries).
    /// Colors:  flat RGB values 0-255 (3267 entries) or empty if no VCLR.
    /// NOTE: int[] not byte[] — System.Text.Json encodes byte[] as Base64, breaking C++ parsing.
    /// </summary>
    private record LandData
    {
        public float[] Heights { get; init; } = [];
        public int[]   Colors  { get; init; } = [];
    }

    private static bool MatchesWorldspaceQuery(IWorldspaceGetter ws, string query)
    {
        if (string.IsNullOrWhiteSpace(query)) return true;
        var q = query.Trim();
        return ws.EditorID?.Contains(q, StringComparison.OrdinalIgnoreCase) == true
            || ws.Name?.String?.Contains(q, StringComparison.OrdinalIgnoreCase) == true;
    }

    private static WorldspaceRecord ToWorldspaceRecord(IWorldspaceGetter ws) => new()
    {
        FormKey      = ws.FormKey.ToString(),
        EditorId     = ws.EditorID,
        Name         = ws.Name?.String,
        PluginSource = ws.FormKey.ModKey.FileName,
    };
}
