using System.Runtime.InteropServices;

Console.WriteLine("=== SemiPlayer Player Skeleton Test ===");

string mediaPath = args.Length > 0
    ? args[0]
    : throw new ArgumentException("Usage: dotnet run --project tools/smoke/SemiPlayer.HelloTest/SemiPlayer.HelloTest.csproj -- <media-file>");

IntPtr player;
EnsureOk(Native.semi_player_create(out player), "semi_player_create");

try
{
    IntPtr versionPtr = Native.semi_ffmpeg_version_string();
    string? version = Marshal.PtrToStringAnsi(versionPtr);
    Console.WriteLine($"[semi_ffmpeg_version_string] {version}");
    Native.semi_free_string(versionPtr);

    EnsureOk(Native.semi_player_open(player, mediaPath), "semi_player_open");
    EnsureOk(Native.semi_player_play(player), "semi_player_play");
    EnsureOk(Native.semi_player_seek(player, 12_345, 0), "semi_player_seek");
    EnsureOk(Native.semi_player_set_speed(player, 1.25), "semi_player_set_speed");
    EnsureOk(Native.semi_player_set_subtitle_visible(player, 1), "semi_player_set_subtitle_visible");
    EnsureOk(Native.semi_player_pause(player), "semi_player_pause");

    EnsureOk(Native.semi_player_get_state(player, out uint state), "semi_player_get_state");
    EnsureOk(Native.semi_player_get_position_ms(player, out long positionMs), "semi_player_get_position_ms");
    EnsureOk(Native.semi_player_get_duration_ms(player, out long durationMs), "semi_player_get_duration_ms");
    EnsureOk(Native.semi_player_get_media_info(player, out SemiMediaInfo mediaInfo), "semi_player_get_media_info");

    Console.WriteLine($"[state] {state}");
    Console.WriteLine($"[position_ms] {positionMs}");
    Console.WriteLine($"[duration_ms] {durationMs}");
    Console.WriteLine($"[stream_count] {mediaInfo.StreamCount}");
    Console.WriteLine($"[video_stream_count] {mediaInfo.VideoStreamCount}");
    Console.WriteLine($"[audio_stream_count] {mediaInfo.AudioStreamCount}");
    Console.WriteLine($"[subtitle_stream_count] {mediaInfo.SubtitleStreamCount}");
    Console.WriteLine($"[best_video_stream_index] {mediaInfo.BestVideoStreamIndex}");
    Console.WriteLine($"[best_audio_stream_index] {mediaInfo.BestAudioStreamIndex}");
    Console.WriteLine($"[best_subtitle_stream_index] {mediaInfo.BestSubtitleStreamIndex}");
    Console.WriteLine($"[video_width] {mediaInfo.VideoWidth}");
    Console.WriteLine($"[video_height] {mediaInfo.VideoHeight}");
    Console.WriteLine($"[audio_sample_rate] {mediaInfo.AudioSampleRate}");
    Console.WriteLine($"[audio_channels] {mediaInfo.AudioChannels}");

    EnsureOk(Native.semi_player_reset(player), "semi_player_reset");
    Console.WriteLine("=== All player skeleton tests passed ===");
}
finally
{
    Native.semi_player_destroy(player);
}

static void EnsureOk(int code, string api)
{
    if (code != 0)
    {
        throw new InvalidOperationException($"{api} failed with code {code}");
    }
}

internal static class Native
{
    private const string DllName = "semi_player_rs";

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_create(out IntPtr outPlayer);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    internal static extern int semi_player_open(IntPtr player, string pathUtf8);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_play(IntPtr player);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_pause(IntPtr player);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_seek(IntPtr player, long positionMs, int exact);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_reset(IntPtr player);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_set_speed(IntPtr player, double speed);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_set_subtitle_visible(IntPtr player, int visible);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_state(IntPtr player, out uint state);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_position_ms(IntPtr player, out long positionMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_duration_ms(IntPtr player, out long durationMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_media_info(IntPtr player, out SemiMediaInfo mediaInfo);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void semi_player_destroy(IntPtr player);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr semi_ffmpeg_version_string();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void semi_free_string(IntPtr s);
}

[StructLayout(LayoutKind.Sequential)]
internal struct SemiMediaInfo
{
    internal long DurationMs;
    internal uint StreamCount;
    internal uint VideoStreamCount;
    internal uint AudioStreamCount;
    internal uint SubtitleStreamCount;
    internal int BestVideoStreamIndex;
    internal int BestAudioStreamIndex;
    internal int BestSubtitleStreamIndex;
    internal uint VideoWidth;
    internal uint VideoHeight;
    internal uint AudioSampleRate;
    internal ushort AudioChannels;
    internal ushort Reserved0;
}
