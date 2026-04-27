using System.Runtime.InteropServices;
using System.Text;
using HKX2E;

namespace SctBridge;

/// <summary>
/// Native bridge entry points loaded by SCT via .NET hosting.
/// All methods are [UnmanagedCallersOnly] — called directly as C function pointers.
/// </summary>
public static class Bridge
{
    [ThreadStatic]
    private static string? s_lastError;

    // ── HKX → XML ──────────────────────────────────────────────────────────────
    //
    // Deserializes a binary Havok .hkx file and serializes it to Havok packfile XML.
    // On success: *outXml is a CoTaskMem-allocated null-terminated UTF-8 buffer,
    //             *outLen is its length in bytes (excluding the null terminator).
    //             Caller must free it with sct_free_buffer.
    // Returns 0 on success, non-zero on failure (call sct_last_error for details).

    [UnmanagedCallersOnly(EntryPoint = "sct_hkx_to_xml")]
    public static unsafe int HkxToXml(byte* pathUtf8, byte** outXml, int* outLen)
    {
        *outXml = null;
        *outLen = 0;
        try
        {
            var path = Marshal.PtrToStringUTF8((IntPtr)pathUtf8)
                       ?? throw new ArgumentException("null path");

            hkRootLevelContainer root;
            using (var stream = File.OpenRead(path))
            {
                var br  = new BinaryReaderEx(stream);
                var des = new PackFileDeserializer();
                root = (hkRootLevelContainer)des.Deserialize(br);
            }

            using var ms = new MemoryStream();
            var xs = new HavokXmlSerializer();
            xs.Serialize(root, HKXHeader.SkyrimSE(), ms);

            var bytes = ms.ToArray();
            var ptr   = (byte*)Marshal.AllocCoTaskMem(bytes.Length + 1);
            bytes.CopyTo(new Span<byte>(ptr, bytes.Length));
            ptr[bytes.Length] = 0;

            *outXml = ptr;
            *outLen = bytes.Length;
            s_lastError = null;
            return 0;
        }
        catch (Exception ex)
        {
            s_lastError = ex.Message;
            return -1;
        }
    }

    // ── Buffer free ────────────────────────────────────────────────────────────

    [UnmanagedCallersOnly(EntryPoint = "sct_free_buffer")]
    public static unsafe void FreeBuffer(void* ptr)
        => Marshal.FreeCoTaskMem((IntPtr)ptr);

    // ── Error retrieval ────────────────────────────────────────────────────────
    //
    // Writes the last thread-local error as UTF-8 into buf (up to bufLen bytes,
    // including null terminator). Returns the number of bytes written excluding null.

    [UnmanagedCallersOnly(EntryPoint = "sct_last_error")]
    public static unsafe int LastError(byte* buf, int bufLen)
    {
        var msg   = s_lastError ?? "unknown error";
        var bytes = Encoding.UTF8.GetBytes(msg);
        int n     = Math.Min(bytes.Length, bufLen - 1);
        if (n > 0) bytes.AsSpan(0, n).CopyTo(new Span<byte>(buf, n));
        if (bufLen > 0) buf[n] = 0;
        return n;
    }
}
