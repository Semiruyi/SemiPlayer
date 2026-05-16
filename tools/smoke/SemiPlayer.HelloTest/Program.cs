using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Diagnostics;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

internal static class PumpTimingConstants
{
    public const double MinAdaptiveTickIntervalMs = 1.0;
    public const double MaxAdaptiveTickIntervalMs = 33.0;
}

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (!TryParseArguments(args, out string mediaPath, out SmokeOptions options, out string? error))
        {
            MessageBox.Show(
                error ?? "Missing media path.",
                "SemiPlayer.HelloTest",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            return 1;
        }

        if (options.PumpSweep is not null)
        {
            return PumpSweepRunner.Run(mediaPath, options.PumpSweep);
        }

        var app = new Application
        {
            ShutdownMode = ShutdownMode.OnMainWindowClose,
        };

        var window = new PlayerSmokeWindow(mediaPath, options);
        return app.Run(window);
    }

    private static bool TryParseArguments(
        string[] args,
        out string mediaPath,
        out SmokeOptions options,
        out string? error)
    {
        mediaPath = string.Empty;
        options = new SmokeOptions();
        error = null;

        List<double>? sweepIntervalsMs = null;
        int sweepSegmentMs = 3_000;
        uint sweepIterations = DefaultPumpSweepIterations;
        string? sweepLogPath = null;
        PumpSweepMode sweepMode = PumpSweepMode.Fixed;
        PumpSweepDriver sweepDriver = PumpSweepDriver.Ui;

        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--auto-close-ms")
            {
                if (i + 1 >= args.Length || !int.TryParse(args[i + 1], out int parsed))
                {
                    error = "Expected integer after --auto-close-ms.";
                    return false;
                }

                options.AutoCloseMs = parsed;
                i++;
                continue;
            }

            if (args[i] == "--pump-sweep-intervals-ms")
            {
                if (i + 1 >= args.Length)
                {
                    error = "Expected comma-separated list after --pump-sweep-intervals-ms.";
                    return false;
                }

                sweepIntervalsMs = ParseSweepIntervals(args[i + 1], out error);
                if (sweepIntervalsMs is null)
                {
                    return false;
                }

                i++;
                continue;
            }

            if (args[i] == "--pump-sweep-segment-ms")
            {
                if (i + 1 >= args.Length || !int.TryParse(args[i + 1], out sweepSegmentMs) || sweepSegmentMs < 500)
                {
                    error = "Expected integer >= 500 after --pump-sweep-segment-ms.";
                    return false;
                }

                i++;
                continue;
            }

            if (args[i] == "--pump-sweep-iterations")
            {
                if (i + 1 >= args.Length || !uint.TryParse(args[i + 1], out sweepIterations) || sweepIterations == 0)
                {
                    error = "Expected positive integer after --pump-sweep-iterations.";
                    return false;
                }

                i++;
                continue;
            }

            if (args[i] == "--pump-sweep-log")
            {
                if (i + 1 >= args.Length || string.IsNullOrWhiteSpace(args[i + 1]))
                {
                    error = "Expected path after --pump-sweep-log.";
                    return false;
                }

                sweepLogPath = args[i + 1];
                i++;
                continue;
            }

            if (args[i] == "--pump-sweep-mode")
            {
                if (i + 1 >= args.Length || !TryParsePumpSweepMode(args[i + 1], out sweepMode))
                {
                    error = "Expected one of: fixed, adaptive, both after --pump-sweep-mode.";
                    return false;
                }

                i++;
                continue;
            }

            if (args[i] == "--pump-sweep-driver")
            {
                if (i + 1 >= args.Length || !TryParsePumpSweepDriver(args[i + 1], out sweepDriver))
                {
                    error = "Expected one of: ui, worker, both after --pump-sweep-driver.";
                    return false;
                }

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
            error = "Usage: dotnet run --project tools/smoke/SemiPlayer.HelloTest/SemiPlayer.HelloTest.csproj -- <media-file> [--auto-close-ms 1500] [--pump-sweep-intervals-ms 15,12,10,8,6] [--pump-sweep-segment-ms 3000] [--pump-sweep-iterations 32] [--pump-sweep-mode fixed|adaptive|both] [--pump-sweep-driver ui|worker|both] [--pump-sweep-log tools/smoke/pump_sweep.log]";
            return false;
        }

        if (sweepIntervalsMs is { Count: > 0 })
        {
            options.PumpSweep = new PumpSweepOptions(
                sweepIntervalsMs,
                sweepSegmentMs,
                sweepIterations,
                sweepLogPath,
                sweepMode,
                sweepDriver);
            options.AutoCloseMs = null;
        }

        return true;
    }

    private const uint DefaultPumpSweepIterations = 32;

    private static List<double>? ParseSweepIntervals(string raw, out string? error)
    {
        error = null;
        List<double> values = new();

        foreach (string part in raw.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (!double.TryParse(part, out double value) || value <= 0)
            {
                error = $"Invalid pump sweep interval: '{part}'.";
                return null;
            }

            values.Add(value);
        }

        if (values.Count == 0)
        {
            error = "Pump sweep intervals list cannot be empty.";
            return null;
        }

        return values;
    }

    private static bool TryParsePumpSweepMode(string raw, out PumpSweepMode mode)
    {
        switch (raw.Trim().ToLowerInvariant())
        {
            case "fixed":
                mode = PumpSweepMode.Fixed;
                return true;
            case "adaptive":
                mode = PumpSweepMode.Adaptive;
                return true;
            case "both":
                mode = PumpSweepMode.Both;
                return true;
            default:
                mode = PumpSweepMode.Fixed;
                return false;
        }
    }

    private static bool TryParsePumpSweepDriver(string raw, out PumpSweepDriver driver)
    {
        switch (raw.Trim().ToLowerInvariant())
        {
            case "ui":
                driver = PumpSweepDriver.Ui;
                return true;
            case "worker":
                driver = PumpSweepDriver.Worker;
                return true;
            case "both":
                driver = PumpSweepDriver.Both;
                return true;
            default:
                driver = PumpSweepDriver.Ui;
                return false;
        }
    }
}

internal sealed class PlayerSmokeWindow : Window
{
    private const uint StartupPumpIterations = 512;
    private const uint DefaultTickPumpIterations = 32;
    private const double DefaultTickIntervalMs = 15.0;
    private const double MinTickIntervalMs = 4.0;
    private const double MaxTickIntervalMs = 33.0;
    private const uint MinTickPumpIterations = 1;
    private const uint MaxTickPumpIterations = 256;

    private readonly string _mediaPath;
    private readonly SmokeOptions _options;
    private readonly Image _image;
    private readonly TextBlock _statusText;
    private readonly DispatcherTimer _tickTimer;
    private readonly DispatcherTimer? _autoCloseTimer;
    private readonly DispatcherTimer? _pumpSweepTimer;

    private IntPtr _player;
    private bool _isPlayerCreated;
    private bool _isPlaying;
    private long _durationMs;
    private long _lastPresentedPtsMs = long.MinValue;
    private uint _tickPumpIterations = DefaultTickPumpIterations;
    private WriteableBitmap? _bitmap;
    private readonly PlaybackDiagnostics _diagnostics = new();
    private readonly StringBuilder _pumpSweepLog = new();
    private int _pumpSweepIndex = -1;
    private bool _useAdaptivePump = true;
    private bool _drivePumpFromUi;

    public PlayerSmokeWindow(string mediaPath, SmokeOptions options)
    {
        _mediaPath = mediaPath;
        _options = options;

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
            Interval = TimeSpan.FromMilliseconds(DefaultTickIntervalMs),
        };
        _tickTimer.Tick += (_, _) => OnTick();

        if (_options.AutoCloseMs is int closeDelayMs)
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

        if (_options.PumpSweep is not null)
        {
            _pumpSweepTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(_options.PumpSweep.SegmentMs),
            };
            _pumpSweepTimer.Tick += (_, _) => OnPumpSweepTick();
            WindowState = WindowState.Minimized;
            ShowInTaskbar = false;
        }

        _drivePumpFromUi = _options.PumpSweep is not null;

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
            InitializePumpSweepIfNeeded();
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
            if (_drivePumpFromUi)
            {
                EnsureOk(Native.semi_player_pump(_player, _tickPumpIterations), "semi_player_pump");
            }
            RefreshVideoFrame(forceCopy: false);
        }
        catch (Exception ex)
        {
            _tickTimer.Stop();
            _statusText.Text = ex.Message;
        }
    }

    private void InitializePumpSweepIfNeeded()
    {
        if (_options.PumpSweep is null)
        {
            return;
        }

        _tickPumpIterations = _options.PumpSweep.TickPumpIterations;
        _pumpSweepLog.Clear();
        _pumpSweepLog.AppendLine($"Pump sweep for {_mediaPath}");
        _pumpSweepLog.AppendLine($"SegmentMs={_options.PumpSweep.SegmentMs} TickPumpIterations={_tickPumpIterations}");
        StartNextPumpSweepSegment();
        _pumpSweepTimer?.Start();
    }

    private void OnPumpSweepTick()
    {
        if (_options.PumpSweep is null)
        {
            return;
        }

        if (_pumpSweepIndex >= 0 && _pumpSweepIndex < _options.PumpSweep.TickIntervalsMs.Count)
        {
            PumpSweepDiagnosticsSnapshot snapshot = _diagnostics.CreatePumpSweepSnapshot();
            double intervalMs = _options.PumpSweep.TickIntervalsMs[_pumpSweepIndex];
            string line =
                $"interval={intervalMs:F1}ms iterations={_tickPumpIterations} pumpRate={snapshot.PumpsPerSecond:F1}/s " +
                $"coreSyncMean={snapshot.CoreSyncErrorMeanMs:F2}ms absMean={snapshot.CoreSyncErrorAbsMeanMs:F2}ms " +
                $"maxPos={snapshot.CoreSyncErrorMaxPositiveMs}ms maxNeg={snapshot.CoreSyncErrorMaxNegativeMs}ms " +
                $"samples={snapshot.SampleCount}";
            _pumpSweepLog.AppendLine(line);
            Console.WriteLine(line);
        }

        if (!StartNextPumpSweepSegment())
        {
            _pumpSweepTimer?.Stop();
            FinalizePumpSweep();
        }
    }

    private bool StartNextPumpSweepSegment()
    {
        if (_options.PumpSweep is null)
        {
            return false;
        }

        _pumpSweepIndex++;
        if (_pumpSweepIndex >= _options.PumpSweep.TickIntervalsMs.Count)
        {
            return false;
        }

        double intervalMs = _options.PumpSweep.TickIntervalsMs[_pumpSweepIndex];
        _tickTimer.Interval = TimeSpan.FromMilliseconds(intervalMs);
        _diagnostics.Reset();
        _lastPresentedPtsMs = long.MinValue;
        RefreshVideoFrame(forceCopy: false);
        return true;
    }

    private void FinalizePumpSweep()
    {
        string finalLog = _pumpSweepLog.ToString().TrimEnd();
        Console.WriteLine("=== Pump Sweep Summary ===");
        Console.WriteLine(finalLog);

        if (!string.IsNullOrWhiteSpace(_options.PumpSweep?.LogPath))
        {
            string logPath = Path.GetFullPath(_options.PumpSweep.LogPath);
            Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
            File.WriteAllText(logPath, finalLog + Environment.NewLine);
            Console.WriteLine($"Pump sweep log saved to {logPath}");
        }

        Close();
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

        int audioOutputCode = Native.semi_player_get_audio_output_snapshot(_player, out SemiAudioOutputSnapshot audioOutput);
        if (audioOutputCode != 0)
        {
            throw new InvalidOperationException($"semi_player_get_audio_output_snapshot failed with code {audioOutputCode}");
        }

        long audioPositionMs = snapshot.AudioPositionMs;

        if (snapshot.HasCurrentVideoFrame == 0)
        {
            _diagnostics.ObserveTick(
                audioPositionMs: audioPositionMs,
                videoPtsMs: null,
                coreSyncErrorMs: snapshot.CoreSyncErrorMs,
                frameCopied: false,
                isPlaying: _isPlaying,
                pumpTriggered: false);
            _statusText.Text = BuildStatusText(snapshot, audioOutput, null);
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
            coreSyncErrorMs: snapshot.CoreSyncErrorMs,
            frameCopied: shouldCopyFrame,
            isPlaying: _isPlaying,
            pumpTriggered: _drivePumpFromUi);

        ApplyAdaptivePumpInterval(snapshot);

        _statusText.Text = BuildStatusText(snapshot, audioOutput, frameInfo);
    }

    private void ApplyAdaptivePumpInterval(SemiPlaybackSnapshot snapshot)
    {
        if (!_useAdaptivePump || _options.PumpSweep is not null)
        {
            return;
        }

        double nextMs = Math.Clamp(
            snapshot.SuggestedPumpWaitMs <= 0 ? DefaultTickIntervalMs : snapshot.SuggestedPumpWaitMs,
            PumpTimingConstants.MinAdaptiveTickIntervalMs,
            PumpTimingConstants.MaxAdaptiveTickIntervalMs);
        _tickTimer.Interval = TimeSpan.FromMilliseconds(nextMs);
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
        SemiAudioOutputSnapshot audioOutput,
        SemiVideoFrameInfo? frameInfo)
    {
        string state = _isPlaying ? "Playing" : "Paused";
        string framePart = frameInfo is SemiVideoFrameInfo frame
            ? $"Frame {frame.PtsMs} ms  {frame.Width}x{frame.Height}  stride {frame.Stride}  bytes {frame.ByteLen}"
            : "Frame unavailable";
        string audioOutputPart =
            $"Out {audioOutput.ConfiguredSampleRate} Hz/{audioOutput.ConfiguredChannels} ch  " +
            $"Started {audioOutput.Started}  DeviceTiming {audioOutput.HasDeviceTiming}  " +
            $"DevBase {audioOutput.BasePtsMs} ms  DevPlayed {audioOutput.DevicePlayedFrames}";
        string audioBufferPart =
            $"MixBuf {audioOutput.BufferedFrames}/{audioOutput.TargetBufferFrames} frames  " +
            $"DevPending {audioOutput.PendingDeviceFrames}  Submitted {audioOutput.SubmittedFramesTotal}";
        string audioProgressPart =
            $"Rendered {audioOutput.RenderedFramesTotal}  Audible {audioOutput.AudibleFramesTotal}";
        string coreSyncPart =
            $"CoreSync Mean {_diagnostics.CoreSyncErrorMeanMs:F1} ms  " +
            $"AbsMean {_diagnostics.CoreSyncErrorAbsMeanMs:F1} ms  " +
            $"Max+ {_diagnostics.CoreSyncErrorMaxPositiveMs} ms  Max- {_diagnostics.CoreSyncErrorMaxNegativeMs} ms";
        string diagnosticsPart =
            $"UI {_diagnostics.UiTicksPerSecond:F1}/s  Copies {_diagnostics.FrameCopiesPerSecond:F1}/s  " +
            $"Advances {_diagnostics.FrameAdvancesPerSecond:F1}/s  LastStep {_diagnostics.LastVideoStepMs} ms  " +
            $"Pump {_diagnostics.PumpsPerSecond:F1}/s @{_tickTimer.Interval.TotalMilliseconds:F1} ms x {_tickPumpIterations}  " +
            $"Mode {(_useAdaptivePump ? "Adaptive" : "Fixed")}  " +
            $"Driver {(_drivePumpFromUi ? "UI" : "Worker")}  " +
            $"Stalled {(_diagnostics.IsStalled ? $"yes ({_diagnostics.StallDurationMs} ms)" : "no")}";
        string syncLoopPart =
            $"WakeAt {snapshot.NextVideoWakeDeadlineMs} ms  FrameEnd {snapshot.CurrentVideoEffectiveEndMs} ms  " +
            $"AudioRefillAt {snapshot.NextAudioRefillDeadlineMs} ms  PumpAt {snapshot.NextPumpDeadlineMs} ms  " +
            $"SuggestWait {snapshot.SuggestedPumpWaitMs} ms  " +
            $"SyncTicks {snapshot.VideoSyncTicks}  Runs {snapshot.VideoSyncRuns}  " +
            $"Presents {snapshot.VideoSyncPresents}  Drops {snapshot.VideoSyncDrops}  " +
            $"Underflows {snapshot.VideoSyncUnderflows}  LateHits {snapshot.VideoSyncLateHits}";
        string avPart =
            $"Core A-V {snapshot.CoreAVDeltaMs} ms  CoreSyncErr {snapshot.CoreSyncErrorMs} ms  " +
            $"HostOffset {snapshot.HostPresentationOffsetMs} ms  " +
            $"Expected End-to-end A-V {snapshot.ExpectedEndToEndAVDeltaMs} ms";

        return
            $"{Path.GetFileName(_mediaPath)}  |  {state}  |  Duration {_durationMs} ms{Environment.NewLine}" +
            $"AudioPos {snapshot.AudioPositionMs} ms  VideoPos {snapshot.CurrentVideoPtsMs} ms  " +
            $"AudioQ {snapshot.AudioQueueLen}  VideoQ {snapshot.VideoQueueLen}  EOS {snapshot.EndOfStream}{Environment.NewLine}" +
            $"{avPart}{Environment.NewLine}" +
            $"{coreSyncPart}{Environment.NewLine}" +
            $"{syncLoopPart}{Environment.NewLine}" +
            $"{audioOutputPart}{Environment.NewLine}" +
            $"{audioBufferPart}{Environment.NewLine}" +
            $"{audioProgressPart}{Environment.NewLine}" +
            $"{framePart}{Environment.NewLine}" +
            $"{diagnosticsPart}{Environment.NewLine}" +
            "Space Play/Pause  Left/Right Seek 5s  Up/Down PumpHz  +/- PumpIters  A AdaptivePump  P UiPump";
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
                case Key.Up:
                    AdjustTickInterval(-1.0);
                    e.Handled = true;
                    break;
                case Key.Down:
                    AdjustTickInterval(1.0);
                    e.Handled = true;
                    break;
                case Key.A:
                    ToggleAdaptivePump();
                    e.Handled = true;
                    break;
                case Key.P:
                    ToggleUiPumpDriver();
                    e.Handled = true;
                    break;
                case Key.OemPlus:
                case Key.Add:
                    AdjustTickPumpIterations(8);
                    e.Handled = true;
                    break;
                case Key.OemMinus:
                case Key.Subtract:
                    AdjustTickPumpIterations(-8);
                    e.Handled = true;
                    break;
            }
        }
        catch (Exception ex)
        {
            _statusText.Text = ex.Message;
        }
    }

    private void AdjustTickInterval(double deltaMs)
    {
        _useAdaptivePump = false;
        double nextMs = Math.Clamp(_tickTimer.Interval.TotalMilliseconds + deltaMs, MinTickIntervalMs, MaxTickIntervalMs);
        _tickTimer.Interval = TimeSpan.FromMilliseconds(nextMs);
        _diagnostics.ResetCoreSyncStats();
        RefreshVideoFrame(forceCopy: false);
    }

    private void AdjustTickPumpIterations(int delta)
    {
        int next = (int)_tickPumpIterations + delta;
        _tickPumpIterations = (uint)Math.Clamp(next, (int)MinTickPumpIterations, (int)MaxTickPumpIterations);
        _diagnostics.ResetCoreSyncStats();
        RefreshVideoFrame(forceCopy: false);
    }

    private void ToggleAdaptivePump()
    {
        _useAdaptivePump = !_useAdaptivePump;
        _diagnostics.ResetCoreSyncStats();
        RefreshVideoFrame(forceCopy: false);
    }

    private void ToggleUiPumpDriver()
    {
        _drivePumpFromUi = !_drivePumpFromUi;
        _diagnostics.Reset();
        RefreshVideoFrame(forceCopy: false);
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
    private int _pumpsInWindow;
    private long? _lastVideoPtsMs;
    private long _lastAudioPositionMs;
    private long _stallStartMs = -1;
    private long _coreSyncErrorCount;
    private long _coreSyncErrorSumMs;
    private long _coreSyncErrorAbsSumMs;
    private long _coreSyncErrorMaxPositiveMs;
    private long _coreSyncErrorMaxNegativeMs;

    public double UiTicksPerSecond { get; private set; }

    public double FrameCopiesPerSecond { get; private set; }

    public double FrameAdvancesPerSecond { get; private set; }

    public double PumpsPerSecond { get; private set; }

    public long LastVideoStepMs { get; private set; }

    public bool IsStalled { get; private set; }

    public long StallDurationMs { get; private set; }

    public double CoreSyncErrorMeanMs { get; private set; }

    public double CoreSyncErrorAbsMeanMs { get; private set; }

    public long CoreSyncErrorMaxPositiveMs => _coreSyncErrorMaxPositiveMs;

    public long CoreSyncErrorMaxNegativeMs => _coreSyncErrorMaxNegativeMs;

    public long CoreSyncErrorSampleCount => _coreSyncErrorCount;

    public void Reset()
    {
        _windowStartMs = ElapsedMs;
        _ticksInWindow = 0;
        _frameCopiesInWindow = 0;
        _frameAdvancesInWindow = 0;
        _pumpsInWindow = 0;
        _lastVideoPtsMs = null;
        _lastAudioPositionMs = 0;
        _stallStartMs = -1;
        UiTicksPerSecond = 0;
        FrameCopiesPerSecond = 0;
        FrameAdvancesPerSecond = 0;
        PumpsPerSecond = 0;
        LastVideoStepMs = 0;
        IsStalled = false;
        StallDurationMs = 0;
        ResetCoreSyncStats();
    }

    public void ResetCoreSyncStats()
    {
        _coreSyncErrorCount = 0;
        _coreSyncErrorSumMs = 0;
        _coreSyncErrorAbsSumMs = 0;
        _coreSyncErrorMaxPositiveMs = 0;
        _coreSyncErrorMaxNegativeMs = 0;
        CoreSyncErrorMeanMs = 0;
        CoreSyncErrorAbsMeanMs = 0;
    }

    public PumpSweepDiagnosticsSnapshot CreatePumpSweepSnapshot()
    {
        return new PumpSweepDiagnosticsSnapshot(
            "ui",
            "ui",
            PumpsPerSecond,
            CoreSyncErrorMeanMs,
            CoreSyncErrorAbsMeanMs,
            _coreSyncErrorMaxPositiveMs,
            _coreSyncErrorMaxNegativeMs,
            _coreSyncErrorCount);
    }

    public void ObserveTick(long audioPositionMs, long? videoPtsMs, long coreSyncErrorMs, bool frameCopied, bool isPlaying, bool pumpTriggered)
    {
        long nowMs = ElapsedMs;
        _ticksInWindow++;
        if (pumpTriggered)
        {
            _pumpsInWindow++;
        }

        if (frameCopied)
        {
            _frameCopiesInWindow++;
        }

        _coreSyncErrorCount++;
        _coreSyncErrorSumMs += coreSyncErrorMs;
        _coreSyncErrorAbsSumMs += Math.Abs(coreSyncErrorMs);
        if (coreSyncErrorMs > _coreSyncErrorMaxPositiveMs)
        {
            _coreSyncErrorMaxPositiveMs = coreSyncErrorMs;
        }
        if (coreSyncErrorMs < _coreSyncErrorMaxNegativeMs)
        {
            _coreSyncErrorMaxNegativeMs = coreSyncErrorMs;
        }
        CoreSyncErrorMeanMs = _coreSyncErrorCount == 0 ? 0 : (double)_coreSyncErrorSumMs / _coreSyncErrorCount;
        CoreSyncErrorAbsMeanMs = _coreSyncErrorCount == 0 ? 0 : (double)_coreSyncErrorAbsSumMs / _coreSyncErrorCount;

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
            PumpsPerSecond = _pumpsInWindow / windowSeconds;

            _windowStartMs = nowMs;
            _ticksInWindow = 0;
            _frameCopiesInWindow = 0;
            _frameAdvancesInWindow = 0;
            _pumpsInWindow = 0;
        }
    }

    private long ElapsedMs => Environment.TickCount64 - _startTimestamp;
}

internal sealed class SmokeOptions
{
    public int? AutoCloseMs { get; set; }

    public PumpSweepOptions? PumpSweep { get; set; }
}

internal sealed class PumpSweepOptions
{
    public PumpSweepOptions(
        List<double> tickIntervalsMs,
        int segmentMs,
        uint tickPumpIterations,
        string? logPath,
        PumpSweepMode mode,
        PumpSweepDriver driver)
    {
        TickIntervalsMs = tickIntervalsMs;
        SegmentMs = segmentMs;
        TickPumpIterations = tickPumpIterations;
        LogPath = logPath;
        Mode = mode;
        Driver = driver;
    }

    public List<double> TickIntervalsMs { get; }

    public int SegmentMs { get; }

    public uint TickPumpIterations { get; }

    public string? LogPath { get; }

    public PumpSweepMode Mode { get; }

    public PumpSweepDriver Driver { get; }
}

internal enum PumpSweepMode
{
    Fixed,
    Adaptive,
    Both,
}

internal enum PumpSweepDriver
{
    Ui,
    Worker,
    Both,
}

internal readonly record struct PumpSweepDiagnosticsSnapshot(
    string Mode,
    string Driver,
    double PumpsPerSecond,
    double CoreSyncErrorMeanMs,
    double CoreSyncErrorAbsMeanMs,
    long CoreSyncErrorMaxPositiveMs,
    long CoreSyncErrorMaxNegativeMs,
    long SampleCount);

internal static class PumpSweepRunner
{
    private const uint StartupPumpIterations = 512;
    private const int WarmupMs = 500;

    public static int Run(string mediaPath, PumpSweepOptions options)
    {
        IntPtr player = IntPtr.Zero;

        try
        {
            EnsureOk(Native.semi_player_create(out player), "semi_player_create");
            EnsureOk(Native.semi_player_open(player, mediaPath), "semi_player_open");
            EnsureOk(Native.semi_player_get_duration_ms(player, out long durationMs), "semi_player_get_duration_ms");
            EnsureOk(Native.semi_player_pump(player, StartupPumpIterations), "semi_player_pump");
            EnsureOk(Native.semi_player_play(player), "semi_player_play");

            long startPositionMs = Math.Clamp(10_000L, 0L, Math.Max(0L, durationMs - 5_000L));

            StringBuilder summary = new();
            summary.AppendLine($"Pump sweep for {mediaPath}");
            summary.AppendLine($"SegmentMs={options.SegmentMs} WarmupMs={WarmupMs} TickPumpIterations={options.TickPumpIterations} Mode={options.Mode} Driver={options.Driver}");

            foreach (double intervalMs in options.TickIntervalsMs)
            {
                foreach (PumpSweepDriver driver in ExpandDrivers(options.Driver))
                {
                    foreach (PumpSweepMode mode in ExpandModes(options.Mode))
                    {
                        EnsureOk(Native.semi_player_seek(player, startPositionMs, 0), "semi_player_seek");
                        EnsureOk(Native.semi_player_pump(player, StartupPumpIterations), "semi_player_pump");
                        EnsureOk(Native.semi_player_play(player), "semi_player_play");

                        PumpSweepDiagnosticsSnapshot result = RunSegment(
                            player,
                            intervalMs,
                            options.TickPumpIterations,
                            options.SegmentMs,
                            WarmupMs,
                            mode,
                            driver);
                        string line =
                            $"driver={result.Driver} mode={result.Mode} interval={intervalMs:F1}ms iterations={options.TickPumpIterations} pumpRate={result.PumpsPerSecond:F1}/s " +
                            $"coreSyncMean={result.CoreSyncErrorMeanMs:F2}ms absMean={result.CoreSyncErrorAbsMeanMs:F2}ms " +
                            $"maxPos={result.CoreSyncErrorMaxPositiveMs}ms maxNeg={result.CoreSyncErrorMaxNegativeMs}ms " +
                            $"samples={result.SampleCount}";
                        Console.WriteLine(line);
                        summary.AppendLine(line);
                    }
                }
            }

            string finalLog = summary.ToString().TrimEnd();
            Console.WriteLine("=== Pump Sweep Summary ===");
            Console.WriteLine(finalLog);

            if (!string.IsNullOrWhiteSpace(options.LogPath))
            {
                string logPath = Path.GetFullPath(options.LogPath);
                Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
                File.WriteAllText(logPath, finalLog + Environment.NewLine);
                Console.WriteLine($"Pump sweep log saved to {logPath}");
            }

            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 1;
        }
        finally
        {
            if (player != IntPtr.Zero)
            {
                Native.semi_player_destroy(player);
            }
        }
    }

    private static PumpSweepDiagnosticsSnapshot RunSegment(
        IntPtr player,
        double intervalMs,
        uint tickPumpIterations,
        int segmentMs,
        int warmupMs,
        PumpSweepMode mode,
        PumpSweepDriver driver)
    {
        Stopwatch stopwatch = Stopwatch.StartNew();
        TimeSpan interval = TimeSpan.FromMilliseconds(intervalMs);
        TimeSpan nextTick = TimeSpan.Zero;
        long sumMs = 0;
        long absSumMs = 0;
        long maxPositiveMs = 0;
        long maxNegativeMs = 0;
        long sampleCount = 0;
        long pumpCount = 0;

        while (stopwatch.ElapsedMilliseconds < warmupMs + segmentMs)
        {
            TimeSpan remaining = nextTick - stopwatch.Elapsed;
            if (remaining > TimeSpan.Zero)
            {
                Thread.Sleep(remaining);
            }

            if (driver == PumpSweepDriver.Ui)
            {
                EnsureOk(Native.semi_player_pump(player, tickPumpIterations), "semi_player_pump");
                pumpCount++;
            }
            EnsureOk(Native.semi_player_get_playback_snapshot(player, out SemiPlaybackSnapshot snapshot), "semi_player_get_playback_snapshot");

            if (stopwatch.ElapsedMilliseconds >= warmupMs && snapshot.HasCurrentVideoFrame != 0)
            {
                long value = snapshot.CoreSyncErrorMs;
                sumMs += value;
                absSumMs += Math.Abs(value);
                maxPositiveMs = Math.Max(maxPositiveMs, value);
                maxNegativeMs = Math.Min(maxNegativeMs, value);
                sampleCount++;
            }

            TimeSpan nextInterval = mode == PumpSweepMode.Adaptive
                ? TimeSpan.FromMilliseconds(Math.Clamp(
                    snapshot.SuggestedPumpWaitMs <= 0 ? intervalMs : snapshot.SuggestedPumpWaitMs,
                    PumpTimingConstants.MinAdaptiveTickIntervalMs,
                    PumpTimingConstants.MaxAdaptiveTickIntervalMs))
                : interval;
            nextTick += nextInterval;
        }

        double seconds = Math.Max(segmentMs, 1) / 1000.0;
        double meanMs = sampleCount == 0 ? 0 : (double)sumMs / sampleCount;
        double absMeanMs = sampleCount == 0 ? 0 : (double)absSumMs / sampleCount;
        double pumpsPerSecond = pumpCount / ((warmupMs + segmentMs) / 1000.0);

        return new PumpSweepDiagnosticsSnapshot(
            mode.ToString().ToLowerInvariant(),
            driver.ToString().ToLowerInvariant(),
            pumpsPerSecond,
            meanMs,
            absMeanMs,
            maxPositiveMs,
            maxNegativeMs,
            sampleCount);
    }

    private static IEnumerable<PumpSweepMode> ExpandModes(PumpSweepMode mode)
    {
        if (mode == PumpSweepMode.Both)
        {
            yield return PumpSweepMode.Fixed;
            yield return PumpSweepMode.Adaptive;
            yield break;
        }

        yield return mode;
    }

    private static IEnumerable<PumpSweepDriver> ExpandDrivers(PumpSweepDriver driver)
    {
        if (driver == PumpSweepDriver.Both)
        {
            yield return PumpSweepDriver.Ui;
            yield return PumpSweepDriver.Worker;
            yield break;
        }

        yield return driver;
    }

    private static void EnsureOk(int code, string api)
    {
        if (code != 0)
        {
            throw new InvalidOperationException($"{api} failed with code {code}");
        }
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
    internal static extern int semi_player_get_position_ms(IntPtr player, out long positionMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_duration_ms(IntPtr player, out long durationMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_playback_snapshot(IntPtr player, out SemiPlaybackSnapshot snapshot);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_audio_output_snapshot(IntPtr player, out SemiAudioOutputSnapshot snapshot);

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
    internal long AudioPositionMs;
    internal uint AudioQueueLen;
    internal uint VideoQueueLen;
    internal uint HasCurrentVideoFrame;
    internal long CurrentVideoPtsMs;
    internal long CurrentVideoDurationMs;
    internal long CurrentVideoEffectiveEndMs;
    internal long NextVideoWakeDeadlineMs;
    internal long LastAudioPtsMs;
    internal int HostPresentationOffsetMs;
    internal long CoreAVDeltaMs;
    internal long CoreSyncErrorMs;
    internal long ExpectedEndToEndAVDeltaMs;
    internal ulong VideoSyncTicks;
    internal ulong VideoSyncRuns;
    internal ulong VideoSyncPresents;
    internal ulong VideoSyncDrops;
    internal ulong VideoSyncUnderflows;
    internal ulong VideoSyncLateHits;
    internal long SuggestedPumpWaitMs;
    internal long NextAudioRefillDeadlineMs;
    internal long NextPumpDeadlineMs;
    internal uint EndOfStream;
}

[StructLayout(LayoutKind.Sequential)]
internal struct SemiAudioOutputSnapshot
{
    internal uint ConfiguredSampleRate;
    internal ushort ConfiguredChannels;
    internal ushort Reserved0;
    internal uint TargetBufferFrames;
    internal uint BufferedFrames;
    internal uint PendingDeviceFrames;
    internal ulong RenderedFramesTotal;
    internal ulong AudibleFramesTotal;
    internal ulong SubmittedFramesTotal;
    internal uint Started;
    internal uint HasDeviceTiming;
    internal long BasePtsMs;
    internal ulong DevicePlayedFrames;
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
