using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Diagnostics;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

internal sealed class SmokeAnomalyLogger
{
    private const long CoreSyncErrorWarnThresholdMs = 16;
    private const long StaleAudioDiscardLagWarnThresholdUs = 2_000;
    private const long LockWaitWarnThresholdUs = 8_000;
    private const long WorkerSlipWarnThresholdUs = 5_000;
    private const uint PausedPendingFramesWarnThreshold = 128;
    private static readonly TimeSpan StartupGracePeriod = TimeSpan.FromMilliseconds(1_500);
    private static readonly TimeSpan ThrottleWindow = TimeSpan.FromMilliseconds(500);

    private readonly Dictionary<string, DateTime> _lastLoggedAtUtc = new();
    private readonly HashSet<string> _activeAnomalies = new(StringComparer.Ordinal);
    private DateTime _sessionStartedAtUtc;

    public string LogPath { get; } = Path.GetFullPath(Path.Combine("tools", "smoke", "smoke-anomalies.log"));

    public void ResetSession()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
        File.WriteAllText(LogPath, string.Empty);
        _lastLoggedAtUtc.Clear();
        _activeAnomalies.Clear();
        _sessionStartedAtUtc = DateTime.UtcNow;
    }

    public string CurrentSummary =>
        _activeAnomalies.Count == 0
            ? "none"
            : string.Join(", ", _activeAnomalies);

    public void Observe(SemiPlaybackSnapshot snapshot, PlaybackDiagnostics diagnostics)
    {
        _activeAnomalies.Clear();
        bool afterStartupGrace = DateTime.UtcNow - _sessionStartedAtUtc >= StartupGracePeriod;

        if (afterStartupGrace && Math.Abs(snapshot.CoreSyncErrorMs) >= CoreSyncErrorWarnThresholdMs)
        {
            _activeAnomalies.Add("core-sync");
            LogThrottled(
                "core-sync",
                $"core-sync-error ms={snapshot.CoreSyncErrorMs} mean={diagnostics.CoreSyncErrorMeanMs:F2} absMean={diagnostics.CoreSyncErrorAbsMeanMs:F2} audio={snapshot.AudioPositionMs} video={snapshot.CurrentVideoPtsMs}");
        }

        if (afterStartupGrace && snapshot.StaleAudioDiscardLastLagUs >= StaleAudioDiscardLagWarnThresholdUs)
        {
            _activeAnomalies.Add("stale-audio-discard");
            LogThrottled(
                "stale-audio-discard",
                $"stale-audio-discard events={snapshot.StaleAudioDiscardEventCount} frames={snapshot.StaleAudioDiscardFrameCount} lastFrames={snapshot.StaleAudioDiscardLastFrameCount} lastLagUs={snapshot.StaleAudioDiscardLastLagUs} maxLagUs={snapshot.StaleAudioDiscardMaxLagUs}");
        }

        if (afterStartupGrace
            && (snapshot.FfiLockWaitLastUs >= LockWaitWarnThresholdUs
                || snapshot.SyncWorkerLockWaitLastUs >= LockWaitWarnThresholdUs
                || snapshot.DecodeWorkerLockWaitLastUs >= LockWaitWarnThresholdUs))
        {
            _activeAnomalies.Add("lock-wait");
            LogThrottled(
                "lock-wait",
                $"lock-wait ffiLastUs={snapshot.FfiLockWaitLastUs} ffiMaxUs={snapshot.FfiLockWaitMaxUs} " +
                $"syncLastUs={snapshot.SyncWorkerLockWaitLastUs} syncMaxUs={snapshot.SyncWorkerLockWaitMaxUs} " +
                $"decodeLastUs={snapshot.DecodeWorkerLockWaitLastUs} decodeMaxUs={snapshot.DecodeWorkerLockWaitMaxUs}");
        }

        if (afterStartupGrace && snapshot.WorkerDeadlineSlipLastUs >= WorkerSlipWarnThresholdUs)
        {
            _activeAnomalies.Add("worker-slip");
            LogThrottled(
                "worker-slip",
                $"worker-slip lastUs={snapshot.WorkerDeadlineSlipLastUs} maxUs={snapshot.WorkerDeadlineSlipMaxUs} nextPumpAtMs={snapshot.NextPumpDeadlineMs}");
        }

        if (afterStartupGrace && snapshot.VideoSyncDrops > 0 && snapshot.LastSyncDroppedFrames > 0)
        {
            _activeAnomalies.Add("video-drop");
            LogThrottled(
                "video-drop",
                $"video-drop totalDrops={snapshot.VideoSyncDrops} lastDropped={snapshot.LastSyncDroppedFrames} maxDroppedRun={snapshot.MaxSyncDroppedFrames}");
        }

        if (afterStartupGrace
            && snapshot.AudioOutputStarted == 0
            && snapshot.PendingDeviceFrames > PausedPendingFramesWarnThreshold)
        {
            _activeAnomalies.Add("paused-pending");
            LogThrottled(
                "paused-pending",
                $"paused-pending pendingFrames={snapshot.PendingDeviceFrames} rendered={snapshot.RenderedFramesTotal} audible={snapshot.AudibleFramesTotal}");
        }
    }

    private void LogThrottled(string key, string message)
    {
        DateTime nowUtc = DateTime.UtcNow;
        if (_lastLoggedAtUtc.TryGetValue(key, out DateTime lastUtc) && nowUtc - lastUtc < ThrottleWindow)
        {
            return;
        }

        _lastLoggedAtUtc[key] = nowUtc;
        File.AppendAllText(
            LogPath,
            $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {message}{Environment.NewLine}");
    }
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

            if (args[i] == "--auto-pause-ms")
            {
                if (i + 1 >= args.Length || !int.TryParse(args[i + 1], out int parsed))
                {
                    error = "Expected integer after --auto-pause-ms.";
                    return false;
                }

                options.AutoPauseMs = parsed;
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
            error = "Usage: dotnet run --project tools/smoke/SemiPlayer.HelloTest/SemiPlayer.HelloTest.csproj -- <media-file> [--auto-close-ms 1500] [--auto-pause-ms 1500]";
        }

        return string.IsNullOrWhiteSpace(error);
    }
}

internal sealed class PlayerSmokeWindow : Window
{
    private const double DefaultTickIntervalMs = 15.0;
    private const double MinTickIntervalMs = 4.0;
    private const double MaxTickIntervalMs = 33.0;

    private readonly string _mediaPath;
    private readonly SmokeOptions _options;
    private readonly Image _image;
    private readonly TextBlock _statusText;
    private readonly DispatcherTimer _tickTimer;
    private readonly DispatcherTimer? _autoCloseTimer;
    private readonly DispatcherTimer? _autoPauseTimer;

    private IntPtr _player;
    private bool _isPlayerCreated;
    private bool _isPlaying;
    private long _durationMs;
    private SemiMediaInfo _mediaInfo;
    private long _lastPresentedPtsMs = long.MinValue;
    private WriteableBitmap? _bitmap;
    private byte[]? _frameBuffer;
    private readonly PlaybackDiagnostics _diagnostics = new();
    private readonly SmokeAnomalyLogger _anomalyLogger = new();
    private bool _useAdaptivePump = true;
    private bool _showSeekDebug;
    private SemiVideoPresentationProfile _presentationProfile = SemiVideoPresentationProfile.CpuBgraCompatibility;

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
        if (_options.AutoPauseMs is int pauseDelayMs)
        {
            _autoPauseTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(pauseDelayMs),
            };
            _autoPauseTimer.Tick += (_, _) =>
            {
                _autoPauseTimer.Stop();
                if (_isPlaying)
                {
                    TogglePlayback();
                }
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
            EnsureOk(
                Native.semi_player_set_video_presentation_profile(_player, (uint)_presentationProfile),
                "semi_player_set_video_presentation_profile");
            EnsureOk(Native.semi_player_get_duration_ms(_player, out _durationMs), "semi_player_get_duration_ms");
            EnsureOk(Native.semi_player_get_media_info(_player, out _mediaInfo), "semi_player_get_media_info");
            _diagnostics.Reset();
            _anomalyLogger.ResetSession();

            RefreshVideoFrame(forceCopy: true);

            EnsureOk(Native.semi_player_play(_player), "semi_player_play");
            _isPlaying = true;
            _tickTimer.Start();
            _autoPauseTimer?.Start();
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
                copyDecision: "no-frame",
                isPlaying: _isPlaying);
            _statusText.Text = BuildStatusText(snapshot, audioOutput, null);
            return;
        }

        EnsureOk(Native.semi_player_get_current_video_frame_info(_player, out SemiVideoFrameInfo frameInfo), "semi_player_get_current_video_frame_info");
        EnsureOk(Native.semi_player_get_current_video_surface_desc(_player, out SemiVideoSurfaceDesc surfaceDesc), "semi_player_get_current_video_surface_desc");

        bool cpuReadableSurface = surfaceDesc.Kind == (uint)SemiVideoSurfaceKind.CpuPacked;

        bool shouldCopyFrame =
            cpuReadableSurface &&
            (forceCopy ||
            _bitmap is null ||
            frameInfo.PtsMs != _lastPresentedPtsMs ||
            _bitmap.PixelWidth != frameInfo.Width ||
            _bitmap.PixelHeight != frameInfo.Height);

        string copyDecision = shouldCopyFrame
            ? (forceCopy ? "force" : frameInfo.PtsMs != _lastPresentedPtsMs ? "new-pts" : "resize")
            : cpuReadableSurface ? "same-pts" : "gpu-surface";

        if (shouldCopyFrame)
        {
            byte[] frameBytes = EnsureFrameBuffer(frameInfo.ByteLen);
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
            copyDecision: copyDecision,
            isPlaying: _isPlaying);
        _anomalyLogger.Observe(snapshot, _diagnostics);

        ApplyAdaptivePumpInterval(snapshot);

        _statusText.Text = BuildStatusText(snapshot, audioOutput, frameInfo, surfaceDesc);
    }

    private void ApplyAdaptivePumpInterval(SemiPlaybackSnapshot snapshot)
    {
        if (!_useAdaptivePump)
        {
            return;
        }

        double nextMs = Math.Clamp(
            snapshot.SuggestedPumpWaitMs <= 0 ? DefaultTickIntervalMs : snapshot.SuggestedPumpWaitMs,
            MinTickIntervalMs,
            MaxTickIntervalMs);
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

    private byte[] EnsureFrameBuffer(uint byteLen)
    {
        int requiredLength = checked((int)byteLen);
        if (_frameBuffer is null || _frameBuffer.Length != requiredLength)
        {
            _frameBuffer = new byte[requiredLength];
        }

        return _frameBuffer;
    }

    private string BuildStatusText(
        SemiPlaybackSnapshot snapshot,
        SemiAudioOutputSnapshot audioOutput,
        SemiVideoFrameInfo? frameInfo,
        SemiVideoSurfaceDesc? surfaceDesc = null)
    {
        string state = _isPlaying ? "Playing" : "Paused";
        string sourcePart = BuildSourcePart();
        string overviewLine1 =
            $"{Path.GetFileName(_mediaPath)}  |  {state}  |  Duration {_durationMs} ms";
        string overviewLine2 =
            $"Pos  A {snapshot.AudioPositionMs} ms  V {snapshot.CurrentVideoPtsMs} ms  " +
            $"Q  A {snapshot.AudioQueueLen}  V {snapshot.VideoQueueLen}  EOS {snapshot.EndOfStream}";

        string syncLine1 =
            $"Sync  Core A-V {snapshot.CoreAVDeltaMs} ms  Err {snapshot.CoreSyncErrorMs} ms  " +
            $"Host {snapshot.HostPresentationOffsetMs} ms  End2End {snapshot.ExpectedEndToEndAVDeltaMs} ms";
        string syncLine2 =
            $"SyncStats  Mean {_diagnostics.CoreSyncErrorMeanMs:F1} ms  " +
            $"Abs {_diagnostics.CoreSyncErrorAbsMeanMs:F1} ms  " +
            $"Max+ {_diagnostics.CoreSyncErrorMaxPositiveMs} ms  Max- {_diagnostics.CoreSyncErrorMaxNegativeMs} ms";
        string videoLine =
            $"Video  Cur {snapshot.CurrentVideoPtsMs} ms  Next {snapshot.NextVideoPtsMs} ms  " +
            $"CurEnd {snapshot.CurrentVideoEffectiveEndMs} ms";
        string decodeLine =
            $"Decode  {FormatDecodeBackend(snapshot.VideoDecodeBackend)}  " +
            $"HwReq {snapshot.VideoHardwareRequested}  HwOn {snapshot.VideoHardwareActive}  " +
            $"Fallback {FormatDecodeFallbackReason(snapshot.VideoDecodeFallbackReason)}";
        string surfaceLine =
            surfaceDesc is SemiVideoSurfaceDesc desc
                ? $"Surface  {FormatSurfaceKind(desc.Kind)}  PixFmt {desc.PixelFormat}  " +
                  $"Stride {desc.Stride}  Bytes {desc.ByteLen}  Tex 0x{desc.TexturePtr:X}"
                : $"Surface  {FormatSurfaceKind(snapshot.CurrentVideoSurfaceKind)}  " +
                  $"PixFmt {snapshot.CurrentVideoSurfacePixelFormat}";
        string renderLine =
            $"Render  {FormatPresentationProfile(_presentationProfile)}  " +
            $"Total {snapshot.RenderFramesTotal}  Pass {snapshot.RenderPassthroughFramesTotal}  " +
            $"PassSub {snapshot.RenderPassthroughWithSubtitleIntentFramesTotal}  " +
            $"NeedXform {snapshot.RenderRequiresTransformFramesTotal}  " +
            $"FallbackPass {snapshot.RenderFallbackPassthroughFramesTotal}";

        string audioLine1 =
            $"AudioOut  {audioOutput.ConfiguredSampleRate} Hz/{audioOutput.ConfiguredChannels} ch  " +
            $"Mix {audioOutput.BufferedFrames}/{audioOutput.TargetBufferFrames}  " +
            $"Pending {audioOutput.PendingDeviceFrames}  Started {audioOutput.Started}";
        string audioLine2 =
            $"AudioSched  RefillAt {snapshot.NextAudioRefillDeadlineMs} ms  " +
            $"PumpAt {snapshot.NextPumpDeadlineMs} ms  Wait {snapshot.SuggestedPumpWaitMs} ms";

        string seekLine1 =
            $"Seek  #{snapshot.SeekEventCount} {(snapshot.SeekActive != 0 ? "active" : "done")}  " +
            $"Target {snapshot.LastSeekTargetMs} ms  Api {FormatSeekMetricUs(snapshot.SeekApiDurationUs)}  " +
            $"Lock {FormatSeekMetricUs(snapshot.SeekLockWaitUs)}  Ffmpeg {FormatSeekMetricUs(snapshot.SeekFfmpegSeekUs)}  Reset {FormatSeekMetricUs(snapshot.SeekResetUs)}";
        string seekLine2 =
            $"SeekVideo  First {FormatSeekMetricUs(snapshot.SeekFirstVideoDecodedUs)} @ {FormatSeekPtsMs(snapshot.SeekFirstVideoPtsMs)}  " +
            $"PostT {FormatSeekMetricUs(snapshot.SeekFirstPostTargetVideoDecodedUs)} @ {FormatSeekPtsMs(snapshot.SeekFirstPostTargetVideoPtsMs)}  " +
            $"Ready {FormatSeekMetricUs(snapshot.SeekTargetVideoReadyUs)} @ {FormatSeekPtsMs(snapshot.SeekTargetVideoPtsMs)}  " +
            $"Cur {FormatSeekMetricUs(snapshot.SeekFirstCurrentVideoReadyUs)} @ {FormatSeekPtsMs(snapshot.SeekFirstCurrentVideoPtsMs)}";
        string seekLine3 =
            $"SeekAudio  Raw {FormatSeekMetricUs(snapshot.SeekFirstAudioDecoderOutputUs)}  " +
            $"Play {FormatSeekMetricUs(snapshot.SeekFirstAudioDecodedUs)}  " +
            $"Ready {FormatSeekMetricUs(snapshot.SeekTargetAudioReadyUs)}  " +
            $"EarlyStart {snapshot.SeekAudioOutputStartedBeforeCurrent}";
        string seekLine4 =
            $"SeekSettle  Stable {FormatSeekMetricUs(snapshot.SeekStableUs)}  " +
            $"DropBeforeCur {snapshot.SeekPostTargetVideoDroppedBeforeCurrentCount}  " +
            $"PreTDec {snapshot.SeekPreTargetVideoDecodedCount}  PreTCur {snapshot.SeekPreTargetCurrentVideoCount}";
        string seekLine5 =
            $"SeekDebug  AudioStart {FormatSeekMetricUs(snapshot.SeekAudioOutputStartUs)}  " +
            $"AT@VDec>=T {FormatSeekPtsMs(snapshot.SeekAudioPositionAtFirstPostTargetVideoDecodedMs)}  " +
            $"AT@Cur {FormatSeekPtsMs(snapshot.SeekAudioPositionAtFirstCurrentVideoMs)}  " +
            $"AAdv(VDec>=T->Cur) {FormatSeekPtsMs(snapshot.SeekAudioAdvancedBetweenPostTargetDecodeAndCurrentMs)}";
        string seekLine6 =
            $"SeekDebug  Anchor {FormatSeekPtsMs(snapshot.SeekFirstVideoPacketPtsMs)} / DTS {FormatSeekPtsMs(snapshot.SeekFirstVideoPacketDtsMs)}  " +
            $"Key {(snapshot.SeekFirstVideoPacketIsKey != 0 ? 1 : 0)}  " +
            $"S{snapshot.SeekFirstVideoPacketStreamIndex}/{FormatStreamKind(snapshot.SeekFirstVideoPacketStreamKind)}  " +
            $"dT {FormatSignedMs(snapshot.SeekFirstVideoPacketPtsMs >= 0 ? snapshot.SeekFirstVideoPacketPtsMs - snapshot.LastSeekTargetMs : -1)}  " +
            $"Pkts V{snapshot.SeekVideoPacketsRead} A{snapshot.SeekAudioPacketsRead}";
        string seekLine7 =
            $"SeekDebug  Workload VOut {snapshot.SeekVideoFramesOutput}  VSkip {snapshot.SeekVideoFramesSkipped}  " +
            $"AOut {snapshot.SeekAudioFramesOutput}  ASkip {snapshot.SeekAudioFramesSkipped}";
        string seekLine8 =
            $"SeekDebug  ExpectKF {FormatSeekPtsMs(snapshot.SeekExpectedLeftKeyframePtsMs)} / DTS {FormatSeekPtsMs(snapshot.SeekExpectedLeftKeyframeDtsMs)}  " +
            $"Err {FormatSignedMs(snapshot.SeekExpectedLeftKeyframePtsMs >= 0 && snapshot.SeekFirstVideoPacketPtsMs >= 0 ? snapshot.SeekFirstVideoPacketPtsMs - snapshot.SeekExpectedLeftKeyframePtsMs : -1)}";

        string perfLine =
            $"Perf  UI {_diagnostics.UiTicksPerSecond:F1}/s  Copies {_diagnostics.FrameCopiesPerSecond:F1}/s  " +
            $"Adv {_diagnostics.FrameAdvancesPerSecond:F1}/s  " +
            $"Tick {_tickTimer.Interval.TotalMilliseconds:F1} ms  Mode {(_useAdaptivePump ? "Adaptive" : "Fixed")}";
        string anomalyLine =
            $"Health  Anomalies {_anomalyLogger.CurrentSummary}  " +
            $"Stalled {(_diagnostics.IsStalled ? $"yes ({_diagnostics.StallDurationMs} ms)" : "no")}  " +
            $"AudioDiscardEvents {snapshot.StaleAudioDiscardEventCount}";
        string controlsLine =
            "Space Play/Pause  Left/Right SeekPrevKF/NextKF  Up/Down TickHz  " +
            $"A AdaptiveTick  R Profile({FormatPresentationProfile(_presentationProfile)})  " +
            $"D SeekDebug({(_showSeekDebug ? "On" : "Off")})";

        string statusText =
            $"{overviewLine1}{Environment.NewLine}" +
            $"{overviewLine2}{Environment.NewLine}" +
            $"{sourcePart}{Environment.NewLine}" +
            $"{syncLine1}{Environment.NewLine}" +
            $"{syncLine2}{Environment.NewLine}" +
            $"{videoLine}{Environment.NewLine}" +
            $"{decodeLine}{Environment.NewLine}" +
            $"{surfaceLine}{Environment.NewLine}" +
            $"{renderLine}{Environment.NewLine}" +
            $"{audioLine1}{Environment.NewLine}" +
            $"{audioLine2}{Environment.NewLine}" +
            $"{seekLine1}{Environment.NewLine}" +
            $"{seekLine2}{Environment.NewLine}" +
            $"{seekLine3}{Environment.NewLine}" +
            $"{seekLine4}{Environment.NewLine}" +
            $"{perfLine}{Environment.NewLine}" +
            $"{anomalyLine}{Environment.NewLine}";

        if (_showSeekDebug)
        {
            statusText +=
                $"{seekLine5}{Environment.NewLine}" +
                $"{seekLine6}{Environment.NewLine}" +
                $"{seekLine7}{Environment.NewLine}" +
                $"{seekLine8}{Environment.NewLine}";
        }

        statusText += controlsLine;
        return statusText;

    }

    private static string FormatSeekMetricUs(long valueUs)
    {
        if (valueUs < 0)
        {
            return "n/a";
        }

        return $"{valueUs / 1000.0:F1} ms";
    }

    private static string FormatSeekPtsMs(long valueMs)
    {
        return valueMs < 0 ? "n/a" : $"{valueMs} ms";
    }

    private static string FormatSignedMs(long valueMs)
    {
        return valueMs < 0 ? $"{valueMs} ms" : $"+{valueMs} ms";
    }

    private static string FormatStreamKind(uint kind) => kind switch
    {
        1 => "V",
        2 => "A",
        3 => "Sub",
        4 => "Data",
        5 => "Att",
        _ => "?",
    };

    private static string FormatSurfaceKind(uint kind) => kind switch
    {
        (uint)SemiVideoSurfaceKind.CpuPacked => "CpuPacked",
        (uint)SemiVideoSurfaceKind.D3d11Texture2D => "D3D11",
        _ => "Unknown",
    };

    private static string FormatDecodeBackend(uint backend) => backend switch
    {
        (uint)SemiVideoDecodeBackend.SoftwareBgra => "SoftwareBgra",
        (uint)SemiVideoDecodeBackend.D3d11va => "D3D11VA",
        _ => "Unknown",
    };

    private static string FormatDecodeFallbackReason(uint reason) => reason switch
    {
        (uint)SemiVideoDecodeFallbackReason.None => "none",
        (uint)SemiVideoDecodeFallbackReason.NoHardwareConfig => "no-hw-config",
        (uint)SemiVideoDecodeFallbackReason.HwDeviceCreateFailed => "hw-device-create",
        (uint)SemiVideoDecodeFallbackReason.HwDeviceContextBindFailed => "hw-device-bind",
        (uint)SemiVideoDecodeFallbackReason.HwDecoderOpenFailed => "hw-open",
        (uint)SemiVideoDecodeFallbackReason.HwDecoderTypeMismatch => "hw-type",
        _ => "unknown",
    };

    private static string FormatPresentationProfile(SemiVideoPresentationProfile profile) => profile switch
    {
        SemiVideoPresentationProfile.Passthrough => "Pass",
        SemiVideoPresentationProfile.CpuBgraCompatibility => "CpuBgra",
        SemiVideoPresentationProfile.D3d11BgraPresenter => "D3D11Bgra",
        _ => "Unknown",
    };

    private string BuildSourcePart()
    {
        if (_mediaInfo.VideoFrameRateNum > 0 && _mediaInfo.VideoFrameRateDen > 0)
        {
            double fps = (double)_mediaInfo.VideoFrameRateNum / _mediaInfo.VideoFrameRateDen;
            return $"Source {_mediaInfo.VideoWidth}x{_mediaInfo.VideoHeight}  AvgFps {fps:F3}  ({_mediaInfo.VideoFrameRateNum}/{_mediaInfo.VideoFrameRateDen})";
        }

        return $"Source {_mediaInfo.VideoWidth}x{_mediaInfo.VideoHeight}  AvgFps unknown";
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
                    SeekPrevKeyframe();
                    e.Handled = true;
                    break;
                case Key.Right:
                    SeekNextKeyframe();
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
                case Key.R:
                    CyclePresentationProfile();
                    e.Handled = true;
                    break;
                case Key.D:
                    ToggleSeekDebug();
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

    private void ToggleAdaptivePump()
    {
        _useAdaptivePump = !_useAdaptivePump;
        _diagnostics.ResetCoreSyncStats();
        RefreshVideoFrame(forceCopy: false);
    }

    private void CyclePresentationProfile()
    {
        _presentationProfile = _presentationProfile switch
        {
            SemiVideoPresentationProfile.Passthrough => SemiVideoPresentationProfile.CpuBgraCompatibility,
            SemiVideoPresentationProfile.CpuBgraCompatibility => SemiVideoPresentationProfile.D3d11BgraPresenter,
            SemiVideoPresentationProfile.D3d11BgraPresenter => SemiVideoPresentationProfile.Passthrough,
            _ => SemiVideoPresentationProfile.CpuBgraCompatibility,
        };
        EnsureOk(
            Native.semi_player_set_video_presentation_profile(_player, (uint)_presentationProfile),
            "semi_player_set_video_presentation_profile");
        _diagnostics.Reset();
        RefreshVideoFrame(forceCopy: false);
    }

    private void ToggleSeekDebug()
    {
        _showSeekDebug = !_showSeekDebug;
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
        RefreshVideoFrame(forceCopy: true);
    }

    private const int KeyframeSeekMinOffsetMs = 3000;

    private void SeekPrevKeyframe()
    {
        EnsureOk(Native.semi_player_seek_prev_keyframe(_player, KeyframeSeekMinOffsetMs), "semi_player_seek_prev_keyframe");
        _diagnostics.Reset();
        _lastPresentedPtsMs = long.MinValue;
        RefreshVideoFrame(forceCopy: true);
    }

    private void SeekNextKeyframe()
    {
        EnsureOk(Native.semi_player_seek_next_keyframe(_player, KeyframeSeekMinOffsetMs), "semi_player_seek_next_keyframe");
        _diagnostics.Reset();
        _lastPresentedPtsMs = long.MinValue;
        RefreshVideoFrame(forceCopy: true);
    }

    private void DisposePlayer()
    {
        _tickTimer.Stop();
        _autoPauseTimer?.Stop();
        _autoCloseTimer?.Stop();

        if (_player != IntPtr.Zero)
        {
            Native.semi_player_destroy(_player);
            _player = IntPtr.Zero;
        }

        _frameBuffer = null;
        _bitmap = null;
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
    private long _coreSyncErrorCount;
    private long _coreSyncErrorSumMs;
    private long _coreSyncErrorAbsSumMs;
    private long _coreSyncErrorMaxPositiveMs;
    private long _coreSyncErrorMaxNegativeMs;
    private long _videoStepCount;
    private long _videoStepSumMs;

    public double UiTicksPerSecond { get; private set; }

    public double FrameCopiesPerSecond { get; private set; }

    public double FrameAdvancesPerSecond { get; private set; }

    public long LastVideoStepMs { get; private set; }

    public bool IsStalled { get; private set; }

    public long StallDurationMs { get; private set; }

    public double CoreSyncErrorMeanMs { get; private set; }

    public double CoreSyncErrorAbsMeanMs { get; private set; }

    public long CoreSyncErrorMaxPositiveMs => _coreSyncErrorMaxPositiveMs;

    public long CoreSyncErrorMaxNegativeMs => _coreSyncErrorMaxNegativeMs;

    public long CoreSyncErrorSampleCount => _coreSyncErrorCount;

    public double AverageVideoStepMs { get; private set; }

    public string LastCopyDecision { get; private set; } = "none";

    public long ForceCopyCount { get; private set; }

    public long NewPtsCopyCount { get; private set; }

    public long ResizeCopyCount { get; private set; }

    public long SamePtsSkipCount { get; private set; }

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
        AverageVideoStepMs = 0;
        LastCopyDecision = "none";
        ForceCopyCount = 0;
        NewPtsCopyCount = 0;
        ResizeCopyCount = 0;
        SamePtsSkipCount = 0;
        _videoStepCount = 0;
        _videoStepSumMs = 0;
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

    public void ObserveTick(long audioPositionMs, long? videoPtsMs, long coreSyncErrorMs, bool frameCopied, string copyDecision, bool isPlaying)
    {
        long nowMs = ElapsedMs;
        _ticksInWindow++;
        LastCopyDecision = copyDecision;

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
                _videoStepCount++;
                _videoStepSumMs += LastVideoStepMs;
                AverageVideoStepMs = _videoStepCount == 0 ? 0 : (double)_videoStepSumMs / _videoStepCount;
            }

            _lastVideoPtsMs = currentVideoPtsMs;
        }

        switch (copyDecision)
        {
            case "force":
                ForceCopyCount++;
                break;
            case "new-pts":
                NewPtsCopyCount++;
                break;
            case "resize":
                ResizeCopyCount++;
                break;
            case "same-pts":
                SamePtsSkipCount++;
                break;
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

internal sealed class SmokeOptions
{
    public int? AutoCloseMs { get; set; }

    public int? AutoPauseMs { get; set; }
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
    internal static extern int semi_player_seek_prev_keyframe(IntPtr player, int minOffsetMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_seek_next_keyframe(IntPtr player, int minOffsetMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_position_ms(IntPtr player, out long positionMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_duration_ms(IntPtr player, out long durationMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_media_info(IntPtr player, out SemiMediaInfo mediaInfo);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_playback_snapshot(IntPtr player, out SemiPlaybackSnapshot snapshot);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_audio_output_snapshot(IntPtr player, out SemiAudioOutputSnapshot snapshot);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_current_video_frame_info(IntPtr player, out SemiVideoFrameInfo frameInfo);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_get_current_video_surface_desc(IntPtr player, out SemiVideoSurfaceDesc surfaceDesc);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_copy_current_video_frame_bgra(IntPtr player, byte[] destination, uint destinationLen);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int semi_player_set_video_presentation_profile(IntPtr player, uint profile);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void semi_player_destroy(IntPtr player);
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
    internal uint VideoFrameRateNum;
    internal uint VideoFrameRateDen;
    internal uint AudioSampleRate;
    internal ushort AudioChannels;
    internal ushort Reserved0;
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
    internal uint VideoDecodeBackend;
    internal uint VideoHardwareRequested;
    internal uint VideoHardwareActive;
    internal uint VideoDecodeFallbackReason;
    internal uint CurrentVideoSurfaceKind;
    internal uint CurrentVideoSurfacePixelFormat;
    internal long CurrentVideoEffectiveEndMs;
    internal long NextVideoPtsMs;
    internal long CurrentToNextVideoDeltaMs;
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
    internal ulong LastSyncPresentedFrames;
    internal ulong LastSyncDroppedFrames;
    internal ulong MaxSyncPresentedFrames;
    internal ulong MaxSyncDroppedFrames;
    internal ulong SyncRunPresentOnlyCount;
    internal ulong SyncRunDropOnlyCount;
    internal ulong SyncRunPresentDropCount;
    internal ulong SyncRunOtherCount;
    internal long SuggestedPumpWaitMs;
    internal long NextAudioRefillDeadlineMs;
    internal long NextPumpDeadlineMs;
    internal long FfiLockWaitLastUs;
    internal long FfiLockWaitMaxUs;
    internal long SyncWorkerLockWaitLastUs;
    internal long SyncWorkerLockWaitMaxUs;
    internal long DecodeWorkerLockWaitLastUs;
    internal long DecodeWorkerLockWaitMaxUs;
    internal long WorkerDeadlineSlipLastUs;
    internal long WorkerDeadlineSlipMaxUs;
    internal ulong StaleAudioDiscardEventCount;
    internal ulong StaleAudioDiscardFrameCount;
    internal ulong StaleAudioDiscardLastFrameCount;
    internal long StaleAudioDiscardLastLagUs;
    internal long StaleAudioDiscardMaxLagUs;
    internal ulong RenderFramesTotal;
    internal ulong RenderPassthroughFramesTotal;
    internal ulong RenderPassthroughWithSubtitleIntentFramesTotal;
    internal ulong RenderRequiresTransformFramesTotal;
    internal ulong RenderFallbackPassthroughFramesTotal;
    internal ulong SeekEventCount;
    internal uint SeekActive;
    internal long LastSeekTargetMs;
    internal long SeekApiDurationUs;
    internal long SeekLockWaitUs;
    internal long SeekFfmpegSeekUs;
    internal long SeekResetUs;
    internal long SeekFirstVideoDecodedUs;
    internal long SeekFirstVideoPtsMs;
    internal long SeekFirstPostTargetVideoDecodedUs;
    internal long SeekFirstPostTargetVideoPtsMs;
    internal long SeekAudioPositionAtFirstPostTargetVideoDecodedMs;
    internal long SeekFirstAudioDecoderOutputUs;
    internal long SeekFirstAudioDecodedUs;
    internal long SeekFirstCurrentVideoReadyUs;
    internal long SeekFirstCurrentVideoPtsMs;
    internal long SeekAudioPositionAtFirstCurrentVideoMs;
    internal long SeekAudioAdvancedBetweenPostTargetDecodeAndCurrentMs;
    internal ulong SeekPostTargetVideoDroppedBeforeCurrentCount;
    internal uint SeekAudioOutputStartedBeforeCurrent;
    internal long SeekAudioOutputStartUs;
    internal long SeekTargetVideoReadyUs;
    internal long SeekTargetVideoPtsMs;
    internal long SeekTargetAudioReadyUs;
    internal long SeekStableUs;
    internal ulong SeekPreTargetVideoDecodedCount;
    internal ulong SeekPreTargetCurrentVideoCount;
    internal long SeekFirstVideoPacketPtsMs;
    internal long SeekFirstVideoPacketDtsMs;
    internal uint SeekFirstVideoPacketIsKey;
    internal long SeekFirstVideoPacketPos;
    internal long SeekFirstVideoPacketStreamIndex;
    internal uint SeekFirstVideoPacketStreamKind;
    internal ulong SeekVideoPacketsRead;
    internal ulong SeekAudioPacketsRead;
    internal ulong SeekVideoFramesOutput;
    internal ulong SeekVideoFramesSkipped;
    internal ulong SeekAudioFramesOutput;
    internal ulong SeekAudioFramesSkipped;
    internal long SeekExpectedLeftKeyframePtsMs;
    internal long SeekExpectedLeftKeyframeDtsMs;
    internal uint AudioOutputStarted;
    internal uint PendingDeviceFrames;
    internal ulong RenderedFramesTotal;
    internal ulong AudibleFramesTotal;
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

[StructLayout(LayoutKind.Sequential)]
internal struct SemiVideoSurfaceDesc
{
    internal uint Kind;
    internal uint PixelFormat;
    internal uint Width;
    internal uint Height;
    internal uint Stride;
    internal uint ByteLen;
    internal uint Flags;
    internal ulong TexturePtr;
    internal ulong SharedHandle;
    internal uint ArraySlice;
    internal uint Reserved0;
}

internal enum SemiVideoSurfaceKind : uint
{
    Unknown = 0,
    CpuPacked = 1,
    D3d11Texture2D = 2,
}

internal enum SemiVideoDecodeBackend : uint
{
    Unknown = 0,
    SoftwareBgra = 1,
    D3d11va = 2,
}

internal enum SemiVideoDecodeFallbackReason : uint
{
    None = 0,
    NoHardwareConfig = 1,
    HwDeviceCreateFailed = 2,
    HwDeviceContextBindFailed = 3,
    HwDecoderOpenFailed = 4,
    HwDecoderTypeMismatch = 5,
}

internal enum SemiVideoPresentationProfile : uint
{
    Passthrough = 0,
    CpuBgraCompatibility = 1,
    D3d11BgraPresenter = 2,
}
