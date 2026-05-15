using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (!TryParseArguments(args, out string mediaPath, out int? autoCloseMs, out string? error))
        {
            MessageBox.Show(
                error ?? "Missing media path.",
                "SemiPlayer.HelloTest",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            return 1;
        }

        var app = new Application
        {
            ShutdownMode = ShutdownMode.OnMainWindowClose,
        };

        var window = new PlayerSmokeWindow(mediaPath, autoCloseMs);
        return app.Run(window);
    }

    private static bool TryParseArguments(
        string[] args,
        out string mediaPath,
        out int? autoCloseMs,
        out string? error)
    {
        mediaPath = string.Empty;
        autoCloseMs = null;
        error = null;

        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--auto-close-ms")
            {
                if (i + 1 >= args.Length || !int.TryParse(args[i + 1], out int parsed))
                {
                    error = "Expected integer after --auto-close-ms.";
                    return false;
                }

                autoCloseMs = parsed;
                i++;
                continue;
            }

            if (string.IsNullOrWhiteSpace(mediaPath))
            {
                mediaPath = args[i];
            }
        }

        if (string.IsNullOrWhiteSpace(mediaPath))
        {
            error = "Usage: dotnet run --project tools/smoke/SemiPlayer.HelloTest/SemiPlayer.HelloTest.csproj -- <media-file> [--auto-close-ms 1500]";
            return false;
        }

        return true;
    }
}

internal sealed class PlayerSmokeWindow : Window
{
    private const uint StartupPumpIterations = 512;
    private const uint TickPumpIterations = 32;

    private readonly string _mediaPath;
    private readonly int? _autoCloseMs;
    private readonly Image _image;
    private readonly TextBlock _statusText;
    private readonly DispatcherTimer _tickTimer;
    private readonly DispatcherTimer? _autoCloseTimer;

    private IntPtr _player;
    private bool _isPlayerCreated;
    private bool _isPlaying;
    private long _durationMs;
    private long _lastPresentedPtsMs = long.MinValue;
    private WriteableBitmap? _bitmap;
    private readonly PlaybackDiagnostics _diagnostics = new();

    public PlayerSmokeWindow(string mediaPath, int? autoCloseMs)
    {
        _mediaPath = mediaPath;
        _autoCloseMs = autoCloseMs;

        Title = $"SemiPlayer Smoke - {Path.GetFileName(mediaPath)}";
        Width = 1280;
        Height = 820;
        MinWidth = 640;
        MinHeight = 420;
        Background = new SolidColorBrush(Color.FromRgb(18, 18, 20));
        WindowStartupLocation = WindowStartupLocation.CenterScreen;

        _image = new Image
        {
            Stretch = Stretch.Uniform,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
        };

        _statusText = new TextBlock
        {
            Margin = new Thickness(12, 8, 12, 10),
            Foreground = new SolidColorBrush(Color.FromRgb(220, 220, 224)),
            FontSize = 13,
            TextWrapping = TextWrapping.Wrap,
        };

        var root = new DockPanel();
        DockPanel.SetDock(_statusText, Dock.Bottom);
        root.Children.Add(_statusText);
        root.Children.Add(_image);
        Content = root;

        _tickTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(15),
        };
        _tickTimer.Tick += (_, _) => OnTick();

        if (_autoCloseMs is int closeDelayMs)
        {
            _autoCloseTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(closeDelayMs),
            };
            _autoCloseTimer.Tick += (_, _) =>
            {
                _autoCloseTimer.Stop();
                Close();
            };
        }

        Loaded += (_, _) => InitializePlayer();
        Closed += (_, _) => DisposePlayer();
        KeyDown += OnWindowKeyDown;
    }

    private void InitializePlayer()
    {
        try
        {
            EnsureOk(Native.semi_player_create(out _player), "semi_player_create");
            _isPlayerCreated = true;

            EnsureOk(Native.semi_player_open(_player, _mediaPath), "semi_player_open");
            EnsureOk(Native.semi_player_get_duration_ms(_player, out _durationMs), "semi_player_get_duration_ms");
            _diagnostics.Reset();

            EnsureOk(Native.semi_player_pump(_player, StartupPumpIterations), "semi_player_pump");
            RefreshVideoFrame(forceCopy: true);

            EnsureOk(Native.semi_player_play(_player), "semi_player_play");
            _isPlaying = true;
            _tickTimer.Start();
            _autoCloseTimer?.Start();
        }
        catch (Exception ex)
        {
            _statusText.Text = ex.Message;
            MessageBox.Show(
                ex.Message,
                "SemiPlayer.HelloTest",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            Close();
        }
    }

    private void OnTick()
    {
        if (!_isPlayerCreated)
        {
            return;
        }

        try
        {
            EnsureOk(Native.semi_player_pump(_player, TickPumpIterations), "semi_player_pump");
            RefreshVideoFrame(forceCopy: false);
        }
        catch (Exception ex)
        {
            _tickTimer.Stop();
            _statusText.Text = ex.Message;
        }
    }

    private void RefreshVideoFrame(bool forceCopy)
    {
        if (!_isPlayerCreated)
        {
            return;
        }

        int snapshotCode = Native.semi_player_get_playback_snapshot(_player, out SemiPlaybackSnapshot snapshot);
        if (snapshotCode != 0)
        {
            throw new InvalidOperationException($"semi_player_get_playback_snapshot failed with code {snapshotCode}");
        }

        EnsureOk(Native.semi_player_get_position_ms(_player, out long audioPositionMs), "semi_player_get_position_ms");

        if (snapshot.HasCurrentVideoFrame == 0)
        {
            _diagnostics.ObserveTick(
                audioPositionMs: audioPositionMs,
                videoPtsMs: null,
                frameCopied: false,
                isPlaying: _isPlaying);
            _statusText.Text = BuildStatusText(snapshot, null, audioPositionMs);
            return;
        }

        EnsureOk(Native.semi_player_get_current_video_frame_info(_player, out SemiVideoFrameInfo frameInfo), "semi_player_get_current_video_frame_info");

        bool shouldCopyFrame =
            forceCopy ||
            _bitmap is null ||
            frameInfo.PtsMs != _lastPresentedPtsMs ||
            _bitmap.PixelWidth != frameInfo.Width ||
            _bitmap.PixelHeight != frameInfo.Height;

        if (shouldCopyFrame)
        {
            byte[] frameBytes = new byte[frameInfo.ByteLen];
            EnsureOk(
                Native.semi_player_copy_current_video_frame_bgra(_player, frameBytes, frameInfo.ByteLen),
                "semi_player_copy_current_video_frame_bgra");

            EnsureBitmap(frameInfo);
            _bitmap!.WritePixels(
                new Int32Rect(0, 0, (int)frameInfo.Width, (int)frameInfo.Height),
                frameBytes,
                (int)frameInfo.Stride,
                0);

            _lastPresentedPtsMs = frameInfo.PtsMs;
        }

        _diagnostics.ObserveTick(
            audioPositionMs: audioPositionMs,
            videoPtsMs: frameInfo.PtsMs,
            frameCopied: shouldCopyFrame,
            isPlaying: _isPlaying);

        _statusText.Text = BuildStatusText(snapshot, frameInfo, audioPositionMs);
    }

    private void EnsureBitmap(SemiVideoFrameInfo frameInfo)
    {
        if (_bitmap is not null &&
            _bitmap.PixelWidth == frameInfo.Width &&
            _bitmap.PixelHeight == frameInfo.Height)
        {
            return;
        }

        _bitmap = new WriteableBitmap(
            (int)frameInfo.Width,
            (int)frameInfo.Height,
            96,
            96,
            PixelFormats.Bgra32,
            null);

        _image.Source = _bitmap;
    }

    private string BuildStatusText(
        SemiPlaybackSnapshot snapshot,
        SemiVideoFrameInfo? frameInfo,
        long audioPositionMs)
    {
        string state = _isPlaying ? "Playing" : "Paused";
        string framePart = frameInfo is SemiVideoFrameInfo frame
            ? $"Frame {frame.PtsMs} ms  {frame.Width}x{frame.Height}  stride {frame.Stride}  bytes {frame.ByteLen}"
            : "Frame unavailable";
        string diagnosticsPart =
            $"UI {_diagnostics.UiTicksPerSecond:F1}/s  Copies {_diagnostics.FrameCopiesPerSecond:F1}/s  " +
            $"Advances {_diagnostics.FrameAdvancesPerSecond:F1}/s  LastStep {_diagnostics.LastVideoStepMs} ms  " +
            $"Stalled {(_diagnostics.IsStalled ? $"yes ({_diagnostics.StallDurationMs} ms)" : "no")}";
        long avDeltaMs = frameInfo is SemiVideoFrameInfo currentFrame
            ? audioPositionMs - currentFrame.PtsMs
            : 0;

        return
            $"{Path.GetFileName(_mediaPath)}  |  {state}  |  Duration {_durationMs} ms{Environment.NewLine}" +
            $"AudioPos {audioPositionMs} ms  VideoPos {snapshot.CurrentVideoPtsMs} ms  A-V {avDeltaMs} ms  " +
            $"AudioQ {snapshot.AudioQueueLen}  VideoQ {snapshot.VideoQueueLen}  EOS {snapshot.EndOfStream}{Environment.NewLine}" +
            $"{framePart}{Environment.NewLine}" +
            $"{diagnosticsPart}{Environment.NewLine}" +
            "Space Play/Pause  Left/Right Seek 5s";
    }

    private void OnWindowKeyDown(object sender, KeyEventArgs e)
    {
        if (!_isPlayerCreated)
        {
            return;
        }

        try
        {
            switch (e.Key)
            {
                case Key.Space:
                    TogglePlayback();
                    e.Handled = true;
                    break;
                case Key.Left:
                    SeekRelative(-5_000);
                    e.Handled = true;
                    break;
                case Key.Right:
                    SeekRelative(5_000);
                    e.Handled = true;
                    break;
            }
        }
        catch (Exception ex)
        {
            _statusText.Text = ex.Message;
        }
    }

    private void TogglePlayback()
    {
        if (_isPlaying)
        {
            EnsureOk(Native.semi_player_pause(_player), "semi_player_pause");
            _isPlaying = false;
        }
        else
        {
            EnsureOk(Native.semi_player_play(_player), "semi_player_play");
            _isPlaying = true;
        }

        RefreshVideoFrame(forceCopy: false);
    }

    private void SeekRelative(long deltaMs)
    {
        EnsureOk(Native.semi_player_get_position_ms(_player, out long positionMs), "semi_player_get_position_ms");
        long targetMs = Math.Clamp(positionMs + deltaMs, 0, _durationMs);
        EnsureOk(Native.semi_player_seek(_player, targetMs, 0), "semi_player_seek");
        _diagnostics.Reset();
        _lastPresentedPtsMs = long.MinValue;
        EnsureOk(Native.semi_player_pump(_player, StartupPumpIterations), "semi_player_pump");
        RefreshVideoFrame(forceCopy: true);
    }

    private void DisposePlayer()
    {
        _tickTimer.Stop();
        _autoCloseTimer?.Stop();

        if (_player != IntPtr.Zero)
        {
            Native.semi_player_destroy(_player);
            _player = IntPtr.Zero;
        }

        _isPlayerCreated = false;
    }

    private static void EnsureOk(int code, string api)
    {
        if (code != 0)
        {
            throw new InvalidOperationException($"{api} failed with code {code}");
        }
    }
}

internal sealed class PlaybackDiagnostics
{
    private readonly long _startTimestamp = Environment.TickCount64;
    private long _windowStartMs;
    private int _ticksInWindow;
    private int _frameCopiesInWindow;
    private int _frameAdvancesInWindow;
    private long? _lastVideoPtsMs;
    private long _lastAudioPositionMs;
    private long _stallStartMs = -1;

    public double UiTicksPerSecond { get; private set; }

    public double FrameCopiesPerSecond { get; private set; }

    public double FrameAdvancesPerSecond { get; private set; }

    public long LastVideoStepMs { get; private set; }

    public bool IsStalled { get; private set; }

    public long StallDurationMs { get; private set; }

    public void Reset()
    {
        _windowStartMs = ElapsedMs;
        _ticksInWindow = 0;
        _frameCopiesInWindow = 0;
        _frameAdvancesInWindow = 0;
        _lastVideoPtsMs = null;
        _lastAudioPositionMs = 0;
        _stallStartMs = -1;
        UiTicksPerSecond = 0;
        FrameCopiesPerSecond = 0;
        FrameAdvancesPerSecond = 0;
        LastVideoStepMs = 0;
        IsStalled = false;
        StallDurationMs = 0;
    }

    public void ObserveTick(long audioPositionMs, long? videoPtsMs, bool frameCopied, bool isPlaying)
    {
        long nowMs = ElapsedMs;
        _ticksInWindow++;

        if (frameCopied)
        {
            _frameCopiesInWindow++;
        }

        bool videoAdvanced = false;
        if (videoPtsMs is long currentVideoPtsMs)
        {
            if (_lastVideoPtsMs is long previousVideoPtsMs && currentVideoPtsMs != previousVideoPtsMs)
            {
                videoAdvanced = true;
                LastVideoStepMs = currentVideoPtsMs - previousVideoPtsMs;
            }

            _lastVideoPtsMs = currentVideoPtsMs;
        }

        if (videoAdvanced)
        {
            _frameAdvancesInWindow++;
            _stallStartMs = -1;
            IsStalled = false;
            StallDurationMs = 0;
        }
        else if (!isPlaying || videoPtsMs is null)
        {
            _stallStartMs = -1;
            IsStalled = false;
            StallDurationMs = 0;
        }
        else if (audioPositionMs > _lastAudioPositionMs + 150)
        {
            if (_stallStartMs < 0)
            {
                _stallStartMs = nowMs;
            }

            StallDurationMs = nowMs - _stallStartMs;
            IsStalled = StallDurationMs >= 500;
        }

        _lastAudioPositionMs = audioPositionMs;

        long windowElapsedMs = nowMs - _windowStartMs;
        if (windowElapsedMs >= 1000)
        {
            double windowSeconds = windowElapsedMs / 1000.0;
            UiTicksPerSecond = _ticksInWindow / windowSeconds;
            FrameCopiesPerSecond = _frameCopiesInWindow / windowSeconds;
            FrameAdvancesPerSecond = _frameAdvancesInWindow / windowSeconds;

            _windowStartMs = nowMs;
            _ticksInWindow = 0;
            _frameCopiesInWindow = 0;
            _frameAdvancesInWindow = 0;
        }
    }

    private long ElapsedMs => Environment.TickCount64 - _startTimestamp;
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
    internal static extern int semi_player_get_position_ms(IntPtr player, out long positionMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_duration_ms(IntPtr player, out long durationMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_playback_snapshot(IntPtr player, out SemiPlaybackSnapshot snapshot);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_current_video_frame_info(IntPtr player, out SemiVideoFrameInfo frameInfo);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_copy_current_video_frame_bgra(IntPtr player, byte[] destination, uint destinationLen);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_pump(IntPtr player, uint maxIterations);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void semi_player_destroy(IntPtr player);
}

[StructLayout(LayoutKind.Sequential)]
internal struct SemiPlaybackSnapshot
{
    internal uint AudioQueueLen;
    internal uint VideoQueueLen;
    internal uint HasCurrentVideoFrame;
    internal long CurrentVideoPtsMs;
    internal long CurrentVideoDurationMs;
    internal long LastAudioPtsMs;
    internal uint EndOfStream;
}

[StructLayout(LayoutKind.Sequential)]
internal struct SemiVideoFrameInfo
{
    internal long PtsMs;
    internal long DurationMs;
    internal uint Width;
    internal uint Height;
    internal uint Stride;
    internal uint PixelFormat;
    internal uint ByteLen;
    internal uint Flags;
}
